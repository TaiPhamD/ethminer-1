#include <ethminer/buildinfo.h>
#include <libdevcore/Log.h>
#include <ethash/ethash.hpp>

#include "EthStratumClient.h"

#ifdef _WIN32
// Needed for certificates validation on TLS connections
#include <wincrypt.h>
#endif

using boost::asio::ip::tcp;

static string diffToTarget(double diff)
{
    using namespace boost::multiprecision;
    using BigInteger = boost::multiprecision::cpp_int;

    static BigInteger base("0x00000000ffff0000000000000000000000000000000000000000000000000000");
    BigInteger product;

    if (diff == 0)
    {
        product = BigInteger("0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    }
    else
    {
        diff = 1 / diff;

        BigInteger idiff(diff);
        product = base * idiff;

        std::string sdiff = boost::lexical_cast<std::string>(diff);
        size_t ldiff = sdiff.length();
        size_t offset = sdiff.find(".");

        if (offset != std::string::npos)
        {
            // Number of decimal places
            size_t precision = (ldiff - 1) - offset;

            // Effective sequence of decimal places
            string decimals = sdiff.substr(offset + 1);

            // Strip leading zeroes. If a string begins with
            // 0 or 0x boost parser considers it hex
            decimals = decimals.erase(0, decimals.find_first_not_of('0'));

            // Build up the divisor as string - just in case
            // parser does some implicit conversion with 10^precision
            string decimalDivisor = "1";
            decimalDivisor.resize(precision + 1, '0');

            // This is the multiplier for the decimal part
            BigInteger multiplier(decimals);

            // This is the divisor for the decimal part
            BigInteger divisor(decimalDivisor);

            BigInteger decimalproduct;
            decimalproduct = base * multiplier;
            decimalproduct /= divisor;

            // Add the computed decimal part
            // to product
            product += decimalproduct;
        }

    }

    // Normalize to 64 chars hex with "0x" prefix
    stringstream ss;
    ss << "0x" << setw(64) << setfill('0') << std::hex << product;

    string target = ss.str();
    boost::algorithm::to_lower(target);
    return target;
}

EthStratumClient::EthStratumClient(int worktimeout, int responsetimeout)
  : PoolClient(),
    m_worktimeout(worktimeout),
    m_responsetimeout(responsetimeout),
    m_io_service(g_io_service),
    m_io_strand(g_io_service),
    m_socket(nullptr),
    m_workloop_timer(g_io_service),
    m_response_plea_times(64),
    m_txQueue(64),
    m_resolver(g_io_service),
    m_endpoints()
{

    m_jSwBuilder.settings_["indentation"] = "";

    // Initialize workloop_timer to infinite wait
    m_workloop_timer.expires_at(boost::posix_time::pos_infin);
    m_workloop_timer.async_wait(m_io_strand.wrap(boost::bind(
        &EthStratumClient::workloop_timer_elapsed, this, boost::asio::placeholders::error)));
    clear_response_pleas();
}

EthStratumClient::~EthStratumClient()
{
    // Do not stop io service.
    // It's global
}

void EthStratumClient::init_socket()
{
    // Prepare Socket
    if (m_conn->SecLevel() != SecureLevel::NONE)
    {
        boost::asio::ssl::context::method method = boost::asio::ssl::context::tls_client;
        if (m_conn->SecLevel() == SecureLevel::TLS12)
            method = boost::asio::ssl::context::tlsv12;

        boost::asio::ssl::context ctx(method);
        m_securesocket = std::make_shared<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
            m_io_service, ctx);
        m_socket = &m_securesocket->next_layer();

        if (getenv("SSL_NOVERIFY"))
        {
            m_securesocket->set_verify_mode(boost::asio::ssl::verify_none);
        }
        else
        {
            m_securesocket->set_verify_mode(boost::asio::ssl::verify_peer);
            m_securesocket->set_verify_callback(
                make_verbose_verification(boost::asio::ssl::rfc2818_verification(m_conn->Host())));
        }
#ifdef _WIN32
        HCERTSTORE hStore = CertOpenSystemStore(0, "ROOT");
        if (hStore == nullptr)
        {
            return;
        }

        X509_STORE* store = X509_STORE_new();
        PCCERT_CONTEXT pContext = nullptr;
        while ((pContext = CertEnumCertificatesInStore(hStore, pContext)) != nullptr)
        {
            X509* x509 = d2i_X509(
                nullptr, (const unsigned char**)&pContext->pbCertEncoded, pContext->cbCertEncoded);
            if (x509 != nullptr)
            {
                X509_STORE_add_cert(store, x509);
                X509_free(x509);
            }
        }

        CertFreeCertificateContext(pContext);
        CertCloseStore(hStore, 0);

        SSL_CTX_set_cert_store(ctx.native_handle(), store);
#else
        char* certPath = getenv("SSL_CERT_FILE");
        try
        {
            ctx.load_verify_file(certPath ? certPath : "/etc/ssl/certs/ca-certificates.crt");
        }
        catch (...)
        {
            cwarn << "Failed to load ca certificates. Either the file "
                     "'/etc/ssl/certs/ca-certificates.crt' does not exist";
            cwarn << "or the environment variable SSL_CERT_FILE is set to an invalid or "
                     "inaccessible file.";
            cwarn << "It is possible that certificate verification can fail.";
        }
#endif
    }
    else
    {
        m_nonsecuresocket = std::make_shared<boost::asio::ip::tcp::socket>(m_io_service);
        m_socket = m_nonsecuresocket.get();
    }

    // Activate keep alive to detect disconnects
    unsigned int keepAlive = 10000;

#if defined(_WIN32)
    int32_t timeout = keepAlive;
    setsockopt(
        m_socket->native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(
        m_socket->native_handle(), SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = keepAlive / 1000;
    tv.tv_usec = keepAlive % 1000;
    setsockopt(m_socket->native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(m_socket->native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

void EthStratumClient::connect()
{
    // Prevent unnecessary and potentially dangerous recursion
    if (m_connecting.load(std::memory_order::memory_order_relaxed))
        return;
    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "EthStratumClient::connect() begin");

    // Start timing operations
    m_workloop_timer.expires_from_now(boost::posix_time::milliseconds(m_workloop_interval));
    m_workloop_timer.async_wait(m_io_strand.wrap(boost::bind(
        &EthStratumClient::workloop_timer_elapsed, this, boost::asio::placeholders::error)));

    // Reset status flags
    m_canconnect.store(false, std::memory_order_relaxed);
    m_connected.store(false, std::memory_order_relaxed);
    m_subscribed.store(false, std::memory_order_relaxed);
    m_authorized.store(false, std::memory_order_relaxed);
    m_authpending.store(false, std::memory_order_relaxed);

    // Reset data for ETHEREUMSTRATUM (NiceHash) mode (if previously used)
    // https://github.com/nicehash/Specifications/blob/master/EthereumStratum_NiceHash_v1.0.0.txt
    /*
    "Before first job (work) is provided, pool MUST set difficulty by sending mining.set_difficulty
    If pool does not set difficulty before first job, then miner can assume difficulty 1 was being
    set." Those above statement imply we MAY NOT receive difficulty thus at each new connection
    restart from 1
    */
    m_nextWorkBoundary = h256("0x00000000ffff0000000000000000000000000000000000000000000000000000");
    m_extraNonce = 0;
    m_extraNonceSizeBytes = 0;

    // Initializes socket and eventually secure stream
    if (!m_socket)
        init_socket();

    // Initialize a new queue of end points
    m_endpoints = std::queue<boost::asio::ip::basic_endpoint<boost::asio::ip::tcp>>();
    m_endpoint = boost::asio::ip::basic_endpoint<boost::asio::ip::tcp>();

    if (m_conn->HostNameType() == dev::UriHostNameType::Dns ||
        m_conn->HostNameType() == dev::UriHostNameType::Basic)
    {
        // Begin resolve all ips associated to hostname
        // calling the resolver each time is useful as most
        // load balancer will give Ips in different order
        m_resolver = tcp::resolver(m_io_service);
        tcp::resolver::query q(m_conn->Host(), toString(m_conn->Port()));

        // Start resolving async
        m_resolver.async_resolve(
            q, m_io_strand.wrap(boost::bind(&EthStratumClient::resolve_handler, this,
                   boost::asio::placeholders::error, boost::asio::placeholders::iterator)));
    }
    else
    {
        // No need to use the resolver if host is already an IP address
        m_endpoints.push(boost::asio::ip::tcp::endpoint(
            boost::asio::ip::address::from_string(m_conn->Host()), m_conn->Port()));
        m_io_service.post(m_io_strand.wrap(boost::bind(&EthStratumClient::start_connect, this)));
    }

    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "EthStratumClient::connect() end");
}

void EthStratumClient::disconnect()
{
    // Prevent unnecessary recursion
    if (!m_connected.load(std::memory_order_relaxed) ||
        m_disconnecting.load(std::memory_order_relaxed))
        return;

    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "EthStratumClient::disconnect() begin");
    m_disconnecting.store(true, std::memory_order_relaxed);

    // Cancel any outstanding async operation
    if (m_socket)
        m_socket->cancel();

    if (m_socket && m_socket->is_open())
    {
        try
        {
            boost::system::error_code sec;

            if (m_conn->SecLevel() != SecureLevel::NONE)
            {
                // This will initiate the exchange of "close_notify" message among parties.
                // If both client and server are connected then we expect the handler with success
                // As there may be a connection issue we also endorse a timeout
                m_securesocket->async_shutdown(
                    m_io_strand.wrap(boost::bind(&EthStratumClient::onSSLShutdownCompleted, this,
                        boost::asio::placeholders::error)));
                enqueue_response_plea();


                // Rest of disconnection is performed asynchronously
                DEV_BUILD_LOG_PROGRAMFLOW(cnote, "EthStratumClient::disconnect() end");
                return;
            }
            else
            {
                m_nonsecuresocket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, sec);
                m_socket->close();
            }
        }
        catch (std::exception const& _e)
        {
            cwarn << "Error while disconnecting:" << _e.what();
        }
    }

    disconnect_finalize();
    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "EthStratumClient::disconnect() end");
}

void EthStratumClient::disconnect_finalize()
{
    if (m_conn->SecLevel() != SecureLevel::NONE)
    {
        if (m_securesocket->lowest_layer().is_open())
        {
            // Manage error code if layer is already shut down
            boost::system::error_code ec;
            m_securesocket->lowest_layer().shutdown(
                boost::asio::ip::tcp::socket::shutdown_both, ec);
            m_securesocket->lowest_layer().close();
        }
        m_securesocket = nullptr;
        m_socket = nullptr;
    }
    else
    {
        m_socket = nullptr;
        m_nonsecuresocket = nullptr;
    }

    // Release locking flag and set connection status
#ifdef DEV_BUILD
    if (g_logOptions & LOG_CONNECT)
        cnote << "Socket disconnected from " << ActiveEndPoint();
#endif
    m_connected.store(false, std::memory_order_relaxed);
    m_subscribed.store(false, std::memory_order_relaxed);
    m_authorized.store(false, std::memory_order_relaxed);
    m_authpending.store(false, std::memory_order_relaxed);
    m_disconnecting.store(false, std::memory_order_relaxed);
    m_txPending.store(false, std::memory_order_relaxed);

    if (!m_conn->IsUnrecoverable())
    {
        // If we got disconnected during autodetection phase
        // reissue a connect lowering stratum mode checks
        // m_canconnect flag is used to prevent never-ending loop when
        // remote endpoint rejects connections attempts persistently since the first
        if (!m_conn->StratumModeConfirmed() && m_canconnect.load(std::memory_order_relaxed))
        {
            // Repost a new connection attempt and advance to next stratum test
            if (m_conn->StratumMode() > 0)
            {
                m_conn->SetStratumMode(m_conn->StratumMode() - 1);
                m_io_service.post(
                    m_io_strand.wrap(boost::bind(&EthStratumClient::start_connect, this)));
                return;
            }
            else
            {
                // There are no more stratum modes to test
                // Mark connection as unrecoverable and trash it
                m_conn->MarkUnrecoverable();
            }
        }
    }

    // Clear plea queue and stop timing
    clear_response_pleas();
    m_solution_submitted_max_id = 0;

    // Put the actor back to sleep
    m_workloop_timer.expires_at(boost::posix_time::pos_infin);
    m_workloop_timer.async_wait(m_io_strand.wrap(boost::bind(
        &EthStratumClient::workloop_timer_elapsed, this, boost::asio::placeholders::error)));

    // Trigger handlers
    if (m_onDisconnected)
        m_onDisconnected();
}

void EthStratumClient::resolve_handler(
    const boost::system::error_code& ec, tcp::resolver::iterator i)
{
    if (!ec)
    {
        while (i != tcp::resolver::iterator())
        {
            m_endpoints.push(i->endpoint());
            i++;
        }
        m_resolver.cancel();

        // Resolver has finished so invoke connection asynchronously
        m_io_service.post(m_io_strand.wrap(boost::bind(&EthStratumClient::start_connect, this)));
    }
    else
    {
        cwarn << "Could not resolve host " << m_conn->Host() << ", " << ec.message();

        // Release locking flag and set connection status
        m_connected.store(false, std::memory_order_relaxed);
        m_connecting.store(false, std::memory_order_relaxed);

        // We "simulate" a disconnect, to ensure a fully shutdown state
        if (m_onDisconnected)
            disconnect_finalize();
    }
}

void EthStratumClient::start_connect()
{
    if (m_connecting.load(std::memory_order_relaxed))
        return;
    m_connecting.store(true, std::memory_order::memory_order_relaxed);

    if (!m_endpoints.empty())
    {
        // Pick the first endpoint in list.
        // Eventually endpoints get discarded on connection errors
        m_endpoint = m_endpoints.front();

        // Re-init socket if we need to
        if (m_socket == nullptr)
            init_socket();

#ifdef DEV_BUILD
        if (g_logOptions & LOG_CONNECT)
            cnote << ("Trying " + toString(m_endpoint) + " ...");
#endif

        clear_response_pleas();
        m_connecting.store(true, std::memory_order::memory_order_relaxed);
        enqueue_response_plea();
        m_solution_submitted_max_id = 0;

        // Start connecting async
        if (m_conn->SecLevel() != SecureLevel::NONE)
        {
            m_securesocket->lowest_layer().async_connect(m_endpoint,
                m_io_strand.wrap(boost::bind(&EthStratumClient::connect_handler, this, _1)));
        }
        else
        {
            m_socket->async_connect(m_endpoint,
                m_io_strand.wrap(boost::bind(&EthStratumClient::connect_handler, this, _1)));
        }
    }
    else
    {
        m_connecting.store(false, std::memory_order_relaxed);
        cwarn << "No more IP addresses to try for host: " << m_conn->Host();

        // We "simulate" a disconnect, to ensure a fully shutdown state
        if (m_onDisconnected)
            disconnect_finalize();
    }
}

void EthStratumClient::workloop_timer_elapsed(const boost::system::error_code& ec)
{
    using namespace std::chrono;

    // On timer cancelled or nothing to check for then early exit
    if (ec == boost::asio::error::operation_aborted)
    {
        return;
    }

    if (m_response_pleas_count.load(std::memory_order_relaxed))
    {
        milliseconds response_delay_ms(0);
        steady_clock::time_point response_plea_time(
            m_response_plea_older.load(std::memory_order_relaxed));

        // Check responses while in connection/disconnection phase
        if (isPendingState())
        {
            response_delay_ms =
                duration_cast<milliseconds>(steady_clock::now() - response_plea_time);

            if ((m_responsetimeout * 1000) >= response_delay_ms.count())
            {
                if (m_connecting.load(std::memory_order_relaxed))
                {
                    // The socket is closed so that any outstanding
                    // asynchronous connection operations are cancelled.
                    m_socket->close();
                    return;
                }

                // This is set for SSL disconnection
                if (m_disconnecting.load(std::memory_order_relaxed) &&
                    (m_conn->SecLevel() != SecureLevel::NONE))
                {
                    if (m_securesocket->lowest_layer().is_open())
                    {
                        m_securesocket->lowest_layer().close();
                        return;
                    }
                }
            }
        }

        // Check responses while connected
        if (isConnected())
        {
            response_delay_ms =
                duration_cast<milliseconds>(steady_clock::now() - response_plea_time);

            if (response_delay_ms.count() >= (m_responsetimeout * 1000))
            {
                if (m_conn->StratumModeConfirmed() == false && m_conn->IsUnrecoverable() == false)
                {
                    // Waiting for a response from pool to a login request
                    // Async self send a fake error response
                    Json::Value jRes;
                    jRes["id"] = unsigned(1);
                    jRes["result"] = Json::nullValue;
                    jRes["error"] = true;
                    clear_response_pleas();
                    m_io_service.post(m_io_strand.wrap(
                        boost::bind(&EthStratumClient::processResponse, this, jRes)));
                }
                else
                {
                    // Waiting for a response to solution submission
                    cwarn << "No response received in " << m_responsetimeout << " seconds.";
                    m_endpoints.pop();
                    m_subscribed.store(false, std::memory_order_relaxed);
                    m_authorized.store(false, std::memory_order_relaxed);
                    clear_response_pleas();
                    m_io_service.post(
                        m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
                }
            }

            // Check how old is last job received
            if (duration_cast<seconds>(steady_clock::now() - m_current_timestamp).count() >
                m_worktimeout)
            {
                cwarn << "No new work received in " << m_worktimeout << " seconds.";
                m_endpoints.pop();
                m_subscribed.store(false, std::memory_order_relaxed);
                m_authorized.store(false, std::memory_order_relaxed);
                clear_response_pleas();
                m_io_service.post(
                    m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
            }
        }
    }

    // Resubmit timing operations
    m_workloop_timer.expires_from_now(boost::posix_time::milliseconds(m_workloop_interval));
    m_workloop_timer.async_wait(m_io_strand.wrap(boost::bind(
        &EthStratumClient::workloop_timer_elapsed, this, boost::asio::placeholders::error)));
}

void EthStratumClient::connect_handler(const boost::system::error_code& ec)
{
    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "EthStratumClient::connect_handler() begin");

    // Set status completion
    m_connecting.store(false, std::memory_order_relaxed);


    // Timeout has run before or we got error
    if (ec || !m_socket->is_open())
    {
        cwarn << ("Error  " + toString(m_endpoint) + " [ " + (ec ? ec.message() : "Timeout") +
                  " ]");

        // We need to close the socket used in the previous connection attempt
        // before starting a new one.
        // In case of error, in fact, boost does not close the socket
        // If socket is not opened it means we got timed out
        if (m_socket->is_open())
            m_socket->close();

        // Discard this endpoint and try the next available.
        // Eventually is start_connect which will check for an
        // empty list.
        m_endpoints.pop();
        m_canconnect.store(false, std::memory_order_relaxed);
        m_io_service.post(m_io_strand.wrap(boost::bind(&EthStratumClient::start_connect, this)));

        DEV_BUILD_LOG_PROGRAMFLOW(cnote, "EthStratumClient::connect_handler() end1");
        return;
    }

    // We got a socket connection established
    // Start a new session of data
    m_canconnect.store(true, std::memory_order_relaxed);
    m_connected.store(true, std::memory_order_relaxed);
    m_current_timestamp = std::chrono::steady_clock::now();
    m_message.clear();

    // Clear txqueue
    m_txQueue.consume_all([](std::string* l) { delete l; });

#ifdef DEV_BUILD
    if (g_logOptions & LOG_CONNECT)
        cnote << "Socket connected to " << ActiveEndPoint();
#endif

    if (m_conn->SecLevel() != SecureLevel::NONE)
    {
        boost::system::error_code hec;
        m_securesocket->lowest_layer().set_option(boost::asio::socket_base::keep_alive(true));
        m_securesocket->lowest_layer().set_option(tcp::no_delay(true));

        m_securesocket->handshake(boost::asio::ssl::stream_base::client, hec);

        if (hec)
        {
            cwarn << "SSL/TLS Handshake failed: " << hec.message();
            if (hec.value() == 337047686)
            {  // certificate verification failed
                cwarn << "This can have multiple reasons:";
                cwarn << "* Root certs are either not installed or not found";
                cwarn << "* Pool uses a self-signed certificate";
                cwarn << "* Pool hostname you're connecting to does not match the CN registered "
                         "for the certificate.";
                cwarn << "Possible fixes:";
#ifndef _WIN32
                cwarn << "* Make sure the file '/etc/ssl/certs/ca-certificates.crt' exists and "
                         "is accessible";
                cwarn << "* Export the correct path via 'export "
                         "SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt' to the correct "
                         "file";
                cwarn << "  On most systems you can install the 'ca-certificates' package";
                cwarn << "  You can also get the latest file here: "
                         "https://curl.haxx.se/docs/caextract.html";
#endif
                cwarn << "* Double check hostname in the -P argument.";
                cwarn << "* Disable certificate verification all-together via environment "
                         "variable. See ethminer --help for info about environment variables";
                cwarn << "If you do the latter please be advised you might expose yourself to the "
                         "risk of seeing your shares stolen";
            }

            // This is a fatal error
            // No need to try other IPs as the certificate is based on host-name
            // not ip address. Trying other IPs would end up with the very same error.
            m_canconnect.store(false, std::memory_order_relaxed);
            m_conn->MarkUnrecoverable();
            m_io_service.post(m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
            DEV_BUILD_LOG_PROGRAMFLOW(cnote, "EthStratumClient::connect_handler() end2");
            return;
        }
    }
    else
    {
        m_nonsecuresocket->set_option(boost::asio::socket_base::keep_alive(true));
        m_nonsecuresocket->set_option(tcp::no_delay(true));
    }

    // Clean buffer from any previous stale data
    m_sendBuffer.consume(4096);
    clear_response_pleas();

    // User and worker
    m_user = m_conn->User();
    m_worker = m_conn->Workername();

    /*

    If connection has been set-up with a specific scheme then
    set it's related stratum version as confirmed.

    Otherwise let's go through an autodetection.

    Autodetection process passes all known stratum modes.
    - 1st pass EthStratumClient::ETHEREUMSTRATUM  (2)
    - 2nd pass EthStratumClient::ETHPROXY         (1)
    - 3rd pass EthStratumClient::STRATUM          (0)
    */

    if (m_conn->Version() < 999)
    {
        m_conn->SetStratumMode(m_conn->Version(), true);
    }
    else
    {
        if (!m_conn->StratumModeConfirmed() && m_conn->StratumMode() == 999)
            m_conn->SetStratumMode(2, false);
    }


    Json::Value jReq;
    jReq["id"] = unsigned(1);
    jReq["method"] = "mining.subscribe";
    jReq["params"] = Json::Value(Json::arrayValue);


    switch (m_conn->StratumMode())
    {
    case EthStratumClient::STRATUM:

        jReq["jsonrpc"] = "2.0";

        break;

    case EthStratumClient::ETHPROXY:

        jReq["method"] = "eth_submitLogin";
        if (m_worker.length())
            jReq["worker"] = m_worker;
        jReq["params"].append(m_user + m_conn->Path());

        break;

    case EthStratumClient::ETHEREUMSTRATUM:

        jReq["params"].append(ethminer_get_buildinfo()->project_name_with_version);
        jReq["params"].append("EthereumStratum/1.0.0");

        break;
    }

    // Begin receive data
    recvSocketData();

    /*
    Send first message
    NOTE !!
    It's been tested that f2pool.com does not respond with json error to wrong
    access message (which is needed to autodetect stratum mode).
    IT DOES NOT RESPOND AT ALL !!
    Due to this we need to set a timeout (arbitrary set to 1 second) and
    if no response within that time consider the tentative login failed
    and switch to next stratum mode test
    */
    enqueue_response_plea();
    send(jReq);

    DEV_BUILD_LOG_PROGRAMFLOW(cnote, "EthStratumClient::connect_handler() end");
}

std::string EthStratumClient::processError(Json::Value& responseObject)
{
    std::string retVar;

    if (responseObject.isMember("error") &&
        !responseObject.get("error", Json::Value::null).isNull())
    {
        if (responseObject["error"].isConvertibleTo(Json::ValueType::stringValue))
        {
            retVar = responseObject.get("error", "Unknown error").asString();
        }
        else if (responseObject["error"].isConvertibleTo(Json::ValueType::arrayValue))
        {
            for (auto i : responseObject["error"])
            {
                retVar += i.asString() + " ";
            }
        }
        else if (responseObject["error"].isConvertibleTo(Json::ValueType::objectValue))
        {
            for (Json::Value::iterator i = responseObject["error"].begin();
                 i != responseObject["error"].end(); ++i)
            {
                Json::Value k = i.key();
                Json::Value v = (*i);
                retVar += (std::string)i.name() + ":" + v.asString() + " ";
            }
        }
    }
    else
    {
        retVar = "Unknown error";
    }

    return retVar;
}

void EthStratumClient::processExtranonce(std::string& enonce)
{
    m_extraNonceSizeBytes = enonce.length();

    cnote << "Extranonce set to " EthWhite << enonce << EthReset " (nicehash)";
    enonce.resize(16, '0');
    m_extraNonce = std::stoul(enonce, nullptr, 16);
}

void EthStratumClient::processResponse(Json::Value& responseObject)
{
    // Store jsonrpc version to test against
    int _rpcVer = responseObject.isMember("jsonrpc") ? 2 : 1;

    bool _isNotification = false;  // Whether or not this message is a reply to previous request or
                                   // is a broadcast notification
    bool _isSuccess = false;       // Whether or not this is a succesful or failed response (implies
                                   // _isNotification = false)
    string _errReason = "";        // Content of the error reason
    string _method = "";           // The method of the notification (or request from pool)
    unsigned _id = 0;  // This SHOULD be the same id as the request it is responding to (known
                       // exception is ethermine.org using 999)


    // Retrieve essential values
    _id = responseObject.get("id", unsigned(0)).asUInt();
    _isSuccess = responseObject.get("error", Json::Value::null).empty();
    _errReason = (_isSuccess ? "" : processError(responseObject));
    _method = responseObject.get("method", "").asString();
    _isNotification = (_method != "" || _id == unsigned(0));

    // Notifications of new jobs are like responses to get_work requests
    if (_isNotification && _method == "" && m_conn->StratumMode() == EthStratumClient::ETHPROXY &&
        responseObject["result"].isArray())
    {
        _method = "mining.notify";
    }

    // Very minimal sanity checks
    // - For rpc2 member "jsonrpc" MUST be valued to "2.0"
    // - For responses ... well ... whatever
    // - For notifications I must receive "method" member and a not empty "params" or "result"
    // member
    if ((_rpcVer == 2 && (!responseObject["jsonrpc"].isString() ||
                             responseObject.get("jsonrpc", "") != "2.0")) ||
        (_isNotification && (responseObject["params"].empty() && responseObject["result"].empty())))
    {
        cwarn << "Pool sent an invalid jsonrpc message...";
        cwarn << "Do not blame ethminer for this. Ask pool devs to honor http://www.jsonrpc.org/ "
                 "specifications ";
        cwarn << "Disconnecting...";
        m_subscribed.store(false, std::memory_order_relaxed);
        m_authorized.store(false, std::memory_order_relaxed);
        m_io_service.post(m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
        return;
    }


    // Handle awaited responses to OUR requests (calc response times)
    if (!_isNotification)
    {
        Json::Value jReq;
        Json::Value jResult = responseObject.get("result", Json::Value::null);
        std::chrono::milliseconds response_delay_ms(0);

        if (_id == 1)
        {
            response_delay_ms = dequeue_response_plea();

            /*
            This is the response to very first message after connection.
            I wish I could manage to have different Ids but apparently ethermine.org always replies
            to first message with id=1 regardless the id originally sent.
            */

            // If still in detection phase every failure to
            // to our issued method must lead to a disconnection and
            // reconnection with next available method.
            if (!m_conn->StratumModeConfirmed())
            {
                if (!_isSuccess)
                {
                    // Disconnect and Proceed with next step of autodetection
                    m_io_service.post(
                        m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
                    return;
                }

                switch (m_conn->StratumMode())
                {
                case EthStratumClient::ETHEREUMSTRATUM:

                    // In case of success we also need to verify third parameter of "result" array
                    // member is exactly "EthereumStratum/1.0.0". Otherwise try with another mode
                    if (jResult.isArray() && jResult[0].isArray() && jResult[0].size() == 3 &&
                        jResult[0].get(Json::Value::ArrayIndex(2), "").asString() ==
                            "EthereumStratum/1.0.0")
                    {
                        // ETHEREUMSTRATUM is confirmed
                        m_conn->SetStratumMode(2, true);
                    }
                    else
                    {
                        // Disconnect and Proceed with next step of autodetection ETHPROXY
                        // compatible
                        m_io_service.post(
                            m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
                        return;
                    }

                    break;

                case EthStratumClient::ETHPROXY:

                    // ETHPROXY is confirmed
                    m_conn->SetStratumMode(1, true);

                    break;

                case EthStratumClient::STRATUM:

                    // STRATUM is confirmed
                    m_conn->SetStratumMode(0, true);

                    break;
                }
            }


            // Response to "mining.subscribe"
            // (https://en.bitcoin.it/wiki/Stratum_mining_protocol#mining.subscribe) Result should
            // be an array with multiple dimensions, we only care about the data if
            // EthStratumClient::ETHEREUMSTRATUM
            switch (m_conn->StratumMode())
            {
            case EthStratumClient::STRATUM:

                if (jResult.isArray() && jResult[0].isArray() && jResult[0].size() == 3 &&
                    jResult[0].get(Json::Value::ArrayIndex(2), "").asString() ==
                        "EthereumStratum/1.0.0")
                {
                    _isSuccess = false;
                }
                else
                {
                    cnote << "Stratum mode detected: STRATUM";
                }

                m_subscribed.store(_isSuccess, std::memory_order_relaxed);
                if (!m_subscribed)
                {
                    cnote << "Could not subscribe: " << _errReason;
                    m_conn->MarkUnrecoverable();
                    m_io_service.post(
                        m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
                    return;
                }
                else
                {
                    cnote << "Subscribed!";
                    m_authpending.store(true, std::memory_order_relaxed);
                    jReq["id"] = unsigned(3);
                    jReq["jsonrpc"] = "2.0";
                    jReq["method"] = "mining.authorize";
                    jReq["params"] = Json::Value(Json::arrayValue);
                    jReq["params"].append(m_conn->User() + m_conn->Path());
                    jReq["params"].append(m_conn->Pass());
                    enqueue_response_plea();
                }

                break;

            case EthStratumClient::ETHPROXY:

                cnote << "Stratum mode detected: ETHPROXY Compatible";
                m_subscribed.store(_isSuccess, std::memory_order_relaxed);
                if (!m_subscribed)
                {
                    cnote << "Could not login:" << _errReason;
                    m_conn->MarkUnrecoverable();
                    m_io_service.post(
                        m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
                    return;
                }
                else
                {
                    cnote << "Logged in!";
                    m_authorized.store(true, std::memory_order_relaxed);

                    // If we get here we have a valid application connection
                    // not only a socket connection
                    if (m_onConnected && m_conn->StratumModeConfirmed())
                    {
                        m_current_timestamp = std::chrono::steady_clock::now();
                        m_onConnected();
                    }

                    jReq["id"] = unsigned(5);
                    jReq["method"] = "eth_getWork";
                    jReq["params"] = Json::Value(Json::arrayValue);
                }

                break;

            case EthStratumClient::ETHEREUMSTRATUM:

                cnote << "Stratum mode detected: ETHEREUMSTRATUM (NiceHash)";
                m_subscribed.store(_isSuccess, std::memory_order_relaxed);
                if (!m_subscribed)
                {
                    cnote << "Could not subscribe: " << _errReason;
                    m_conn->MarkUnrecoverable();
                    m_io_service.post(
                        m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
                    return;
                }
                else
                {
                    cnote << "Subscribed to stratum server";

                    if (!jResult.empty() && jResult.isArray())
                    {
                        std::string enonce = jResult.get(Json::Value::ArrayIndex(1), "").asString();
                        processExtranonce(enonce);
                    }

                    // Notify we're ready for extra nonce subscribtion on the fly
                    // reply to this message should not perform any logic
                    jReq["id"] = unsigned(2);
                    jReq["method"] = "mining.extranonce.subscribe";
                    jReq["params"] = Json::Value(Json::arrayValue);
                    send(jReq);

                    // Eventually request authorization
                    m_authpending.store(true, std::memory_order_relaxed);
                    jReq["id"] = unsigned(3);
                    jReq["method"] = "mining.authorize";
                    jReq["params"].append(m_conn->User() + m_conn->Path());
                    jReq["params"].append(m_conn->Pass());
                    enqueue_response_plea();
                }


                break;
            }

            send(jReq);
        }

        else if (_id == 2)
        {
            // This is the response to mining.extranonce.subscribe
            // according to this
            // https://github.com/nicehash/Specifications/blob/master/NiceHash_extranonce_subscribe_extension.txt
            // In all cases, client does not perform any logic when receiving back these replies.
            // With mining.extranonce.subscribe subscription, client should handle extranonce1
            // changes correctly

            // Nothing to do here.
        }

        else if (_id == 3)
        {
            response_delay_ms = dequeue_response_plea();

            // Response to "mining.authorize"
            // (https://en.bitcoin.it/wiki/Stratum_mining_protocol#mining.authorize) Result should
            // be boolean, some pools also throw an error, so _isSuccess can be false Due to this
            // reevaluate _isSuccess

            if (_isSuccess && jResult.isBool())
            {
                _isSuccess = jResult.asBool();
            }

            m_authpending.store(false, std::memory_order_relaxed);
            m_authorized.store(_isSuccess, std::memory_order_relaxed);

            if (!m_authorized)
            {
                cnote << "Worker not authorized " << m_conn->User() << _errReason;
                m_conn->MarkUnrecoverable();
                m_io_service.post(
                    m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
                return;
            }
            else
            {
                cnote << "Authorized worker " + m_conn->User();

                // If we get here we have a valid application connection
                // not only a socket connection
                if (m_onConnected && m_conn->StratumModeConfirmed())
                {
                    m_current_timestamp = std::chrono::steady_clock::now();
                    m_onConnected();
                }
            }
        }

        else if (_id >= 40 && _id <= m_solution_submitted_max_id)
        {
            response_delay_ms = dequeue_response_plea();

            // Response to solution submission mining.submit
            // (https://en.bitcoin.it/wiki/Stratum_mining_protocol#mining.submit) Result should be
            // boolean, some pools also throw an error, so _isSuccess can be false Due to this
            // reevaluate _isSucess

            if (_isSuccess && jResult.isBool())
            {
                _isSuccess = jResult.asBool();
            }

            {
                const unsigned miner_index = _id - 40;
                dequeue_response_plea();
                if (_isSuccess)
                {
                    if (m_onSolutionAccepted)
                    {
                        m_onSolutionAccepted(response_delay_ms, miner_index);
                    }
                }
                else
                {
                    if (m_onSolutionRejected)
                    {
                        cwarn << "Reject reason :"
                              << (_errReason.empty() ? "Unspecified" : _errReason);
                        m_onSolutionRejected(response_delay_ms, miner_index);
                    }
                }
            }
        }

        else if (_id == 5)
        {
            // This is the response we get on first get_work request issued
            // in mode EthStratumClient::ETHPROXY
            // thus we change it to a mining.notify notification
            if (m_conn->StratumMode() == EthStratumClient::ETHPROXY &&
                responseObject["result"].isArray())
            {
                _method = "mining.notify";
                _isNotification = true;
            }
        }

        else if (_id == 9)
        {
            // Response to hashrate submit
            // Shall we do anything ?
            // Hashrate submit is actually out of stratum spec
            if (!_isSuccess)
            {
                cwarn << "Submit hashRate failed: "
                      << (_errReason.empty() ? "Unspecified error" : _errReason);
            }
        }

        else if (_id == 999)
        {
            // This unfortunate case should not happen as none of the outgoing requests is marked
            // with id 999 However it has been tested that ethermine.org responds with this id when
            // error replying to either mining.subscribe (1) or mining.authorize requests (3) To
            // properly handle this situation we need to rely on Subscribed/Authorized states

            response_delay_ms = dequeue_response_plea();

            if (!_isSuccess)
            {
                if (!m_subscribed)
                {
                    // Subscription pending
                    cnote << "Subscription failed:"
                          << (_errReason.empty() ? "Unspecified error" : _errReason);
                    m_io_service.post(
                        m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
                    return;
                }
                else if (m_subscribed && !m_authorized)
                {
                    // Authorization pending
                    cnote << "Worker not authorized:"
                          << (_errReason.empty() ? "Unspecified error" : _errReason);
                    m_io_service.post(
                        m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
                    return;
                }
            };
        }

        else
        {
            cnote << "Got response for unknown message id [" << _id << "] Discarding...";
            return;
        }
    }

    /*


    Handle unsolicited messages FROM pool AKA notifications

    NOTE !
    Do not process any notification unless login validated
    which means we have detected proper stratum mode.

    */

    if (_isNotification && m_conn->StratumModeConfirmed())
    {
        Json::Value jReq;
        Json::Value jPrm;

        unsigned prmIdx;

        if (_method == "mining.notify")
        {
            // Discard jobs if not properly subscribed
            // or if a job for this transmission has already
            // been processed
            if (!m_subscribed.load(std::memory_order_relaxed) || m_newjobprocessed)
            {
                return;
            }

            /*
            Workaround for Nanopool wrong implementation
            see issue # 1348
            */

            if (m_conn->StratumMode() == EthStratumClient::ETHPROXY &&
                responseObject.isMember("result"))
            {
                jPrm = responseObject.get("result", Json::Value::null);
                prmIdx = 0;
            }
            else
            {
                jPrm = responseObject.get("params", Json::Value::null);
                prmIdx = 1;
            }


            if (jPrm.isArray() && !jPrm.empty())
            {
                m_current.job = jPrm.get(Json::Value::ArrayIndex(0), "").asString();

                if (m_conn->StratumMode() == EthStratumClient::ETHEREUMSTRATUM)
                {
                    string sSeedHash = jPrm.get(Json::Value::ArrayIndex(1), "").asString();
                    string sHeaderHash = jPrm.get(Json::Value::ArrayIndex(2), "").asString();

                    if (sHeaderHash != "" && sSeedHash != "")
                    {

                        m_current.seed = h256(sSeedHash);
                        m_current.header = h256(sHeaderHash);
                        m_current.boundary = m_nextWorkBoundary;
                        m_current.startNonce = m_extraNonce;
                        m_current.exSizeBytes = m_extraNonceSizeBytes;
                        m_current_timestamp = std::chrono::steady_clock::now();

                        // This will signal to dispatch the job
                        // at the end of the transmission.
                        m_newjobprocessed = true;
                    }
                }
                else
                {
                    string sHeaderHash = jPrm.get(Json::Value::ArrayIndex(prmIdx++), "").asString();
                    string sSeedHash = jPrm.get(Json::Value::ArrayIndex(prmIdx++), "").asString();
                    string sShareTarget =
                        jPrm.get(Json::Value::ArrayIndex(prmIdx++), "").asString();
                    if (jPrm.size() > 3)
                    {
                        try
                        {
                            m_current.block = std::stoul(
                                jPrm.get(Json::Value::ArrayIndex(prmIdx++), "").asString(), nullptr,
                                16);
                        }
                        catch (const std::exception&)
                        {
                            m_current.block = -1;
                        }
                    }

                    // coinmine.pl fix
                    int l = sShareTarget.length();
                    if (l < 66)
                        sShareTarget = "0x" + string(66 - l, '0') + sShareTarget.substr(2);

                    m_current.seed = h256(sSeedHash);
                    m_current.header = h256(sHeaderHash);
                    m_current.boundary = h256(sShareTarget);
                    m_current_timestamp = std::chrono::steady_clock::now();

                    // This will signal to dispatch the job
                    // at the end of the transmission.
                    m_newjobprocessed = true;
                }
            }
        }
        else if (_method == "mining.set_difficulty")
        {
            if (m_conn->StratumMode() == EthStratumClient::ETHEREUMSTRATUM)
            {
                jPrm = responseObject.get("params", Json::Value::null);
                if (jPrm.isArray())
                {
                    double nextWorkDifficulty =
                        max(jPrm.get(Json::Value::ArrayIndex(0), 1).asDouble(), 0.0001);
                    m_nextWorkBoundary = h256(diffToTarget(nextWorkDifficulty));
                    cnote << "Difficulty set to " EthWhite << nextWorkDifficulty
                          << EthReset " (nicehash) Target : " << m_nextWorkBoundary.hex();
                }
            }
            else
            {
                cwarn << "Invalid mining.set_difficulty rpc method. Disconnecting ...";
                if (m_conn->StratumModeConfirmed())
                {
                    m_conn->MarkUnrecoverable();
                }
                m_io_service.post(
                    m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
            }
        }
        else if (_method == "mining.set_extranonce" &&
                 m_conn->StratumMode() == EthStratumClient::ETHEREUMSTRATUM)
        {
            jPrm = responseObject.get("params", Json::Value::null);
            if (jPrm.isArray())
            {
                std::string enonce = jPrm.get(Json::Value::ArrayIndex(0), "").asString();
                if (!enonce.empty())
                    processExtranonce(enonce);
            }
        }
        else if (_method == "client.get_version")
        {
            jReq["id"] = _id;
            jReq["result"] = ethminer_get_buildinfo()->project_name_with_version;

            if (_rpcVer == 1)
            {
                jReq["error"] = Json::Value::null;
            }
            else if (_rpcVer == 2)
            {
                jReq["jsonrpc"] = "2.0";
            }

            send(jReq);
        }
        else
        {
            cwarn << "Got unknown method [" << _method << "] from pool. Discarding...";

            // Respond back to issuer
            if (_rpcVer == 2)
            {
                jReq["jsonrpc"] = "2.0";
            }
            jReq["id"] = _id;
            jReq["error"] = "Method not found";

            send(jReq);
        }
    }
}

void EthStratumClient::submitHashrate(string const& rate, string const& id)
{

    if (!isConnected())
        return;

    // There is no stratum method to submit the hashrate so we use the rpc variant.
    // Note !!
    // id = 6 is also the id used by ethermine.org and nanopool to push new jobs
    // thus we will be in trouble if we want to check the result of hashrate submission
    // actually change the id from 6 to 9

    Json::Value jReq;
    jReq["id"] = unsigned(9);
    jReq["jsonrpc"] = "2.0";
    if (m_worker.length())
        jReq["worker"] = m_worker;
    jReq["method"] = "eth_submitHashrate";
    jReq["params"] = Json::Value(Json::arrayValue);
    jReq["params"].append(rate);  // Already expressed as hex
    jReq["params"].append(id);    // Already prefixed by 0x

    send(jReq);
}

void EthStratumClient::submitSolution(const Solution& solution)
{
    if (!m_subscribed.load(std::memory_order_relaxed) ||
        !m_authorized.load(std::memory_order_relaxed))
    {
        cwarn << "Solution not submitted. Not authorized.";
        return;
    }

    string nonceHex = toHex(solution.nonce);

    Json::Value jReq;

    unsigned id = 40 + solution.midx;
    jReq["id"] = id;
    m_solution_submitted_max_id = max(m_solution_submitted_max_id, id);
    jReq["method"] = "mining.submit";
    jReq["params"] = Json::Value(Json::arrayValue);

    switch (m_conn->StratumMode())
    {
    case EthStratumClient::STRATUM:

        jReq["jsonrpc"] = "2.0";
        jReq["params"].append(m_conn->User());
        jReq["params"].append(solution.work.job);
        jReq["params"].append("0x" + nonceHex);
        jReq["params"].append("0x" + solution.work.header.hex());
        jReq["params"].append("0x" + solution.mixHash.hex());
        if (m_worker.length())
            jReq["worker"] = m_worker;

        break;

    case EthStratumClient::ETHPROXY:

        jReq["method"] = "eth_submitWork";
        jReq["params"].append("0x" + nonceHex);
        jReq["params"].append("0x" + solution.work.header.hex());
        jReq["params"].append("0x" + solution.mixHash.hex());
        if (m_worker.length())
            jReq["worker"] = m_worker;

        break;

    case EthStratumClient::ETHEREUMSTRATUM:

        jReq["params"].append(m_conn->User());
        jReq["params"].append(solution.work.job);
        jReq["params"].append(nonceHex.substr(solution.work.exSizeBytes));
    }

    enqueue_response_plea();
    send(jReq);

}

void EthStratumClient::recvSocketData()
{
    if (m_conn->SecLevel() != SecureLevel::NONE)
    {
        async_read(*m_securesocket, m_recvBuffer, boost::asio::transfer_at_least(1),
            m_io_strand.wrap(boost::bind(&EthStratumClient::onRecvSocketDataCompleted, this,
                boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)));
    }
    else
    {
        async_read(*m_nonsecuresocket, m_recvBuffer, boost::asio::transfer_at_least(1),
            m_io_strand.wrap(boost::bind(&EthStratumClient::onRecvSocketDataCompleted, this,
                boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)));
    }
}

void EthStratumClient::onRecvSocketDataCompleted(
    const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    // Due to the nature of io_service's queue and
    // the implementation of the loop this event may trigger
    // late after clean disconnection. Check status of connection
    // before triggering all stack of calls

    if (!ec)
    {
        // DO NOT DO THIS !!!!!
        // std::istream is(&m_recvBuffer);
        // std::string message;
        // getline(is, message)
        /*
        There are three reasons :
        1 - Previous async_read_until calls this handler (aside from error codes)
            with the number of bytes in the buffer's get area up to and including
            the delimiter. So we know where to split the line
        2 - Boost's documentation clearly states that after a succesfull
            async_read_until operation the stream buffer MAY contain additional
            data which HAVE to be left in the buffer for subsequent read operations.
            If another delimiter exists in the buffer then it will get caught
            by the next async_read_until()
        3 - std::istream is(&m_recvBuffer) will CONSUME ALL data in the buffer
            thus invalidating the previous point 2
        */

        // Extract received message and free the buffer
        std::string rx_message(
            boost::asio::buffer_cast<const char*>(m_recvBuffer.data()), bytes_transferred);
        m_recvBuffer.consume(bytes_transferred);
        m_message.append(rx_message);

        // Process each line in the transmission
        // NOTE : as multiple jobs may come in with
        // a single transmission only the last will be dispatched
        m_newjobprocessed = false;
        std::string line;
        size_t offset = m_message.find("\n");
        while (offset != string::npos)
        {
            if (offset > 0)
            {
                line = m_message.substr(0, offset);
                boost::trim(line);

                if (!line.empty())
                {
                    // Out received message only for debug purpouses
                    if (g_logOptions & LOG_JSON)
                        cnote << " << " << line;

                    // Test validity of chunk and process
                    Json::Value jMsg;
                    Json::Reader jRdr;
                    if (jRdr.parse(line, jMsg))
                    {
                        // Run in sync so no 2 different async reads may overlap
                        processResponse(jMsg);
                    }
                    else
                    {
                        string what = jRdr.getFormattedErrorMessages();
                        boost::replace_all(what, "\n", " ");
                        cwarn << "Got invalid Json message : " << what;
                    }
                }
            }

            m_message.erase(0, offset + 1);
            offset = m_message.find("\n");
        }

        // There is a new job - dispatch it
        if (m_newjobprocessed)
            if (m_onWorkReceived)
                m_onWorkReceived(m_current);

        // Eventually keep reading from socket
        if (isConnected())
            recvSocketData();
    }
    else
    {
        if (isConnected())
        {
            if (m_authpending.load(std::memory_order_relaxed))
            {
                cwarn << "Error while waiting for authorization from pool";
                cwarn << "Double check your pool credentials.";
                m_conn->MarkUnrecoverable();
            }

            if ((ec.category() == boost::asio::error::get_ssl_category()) &&
                (ERR_GET_REASON(ec.value()) == SSL_RECEIVED_SHUTDOWN))
            {
                cnote << "SSL Stream remotely closed by " << m_conn->Host();
            }
            else if (ec == boost::asio::error::eof)
            {
                cnote << "Connection remotely closed by " << m_conn->Host();
            }
            else
            {
                cwarn << "Socket read failed: " << ec.message();
            }
            m_io_service.post(m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
        }
    }
}

void EthStratumClient::send(Json::Value const& jReq)
{
    std::string* line = new std::string(Json::writeString(m_jSwBuilder, jReq));
    m_txQueue.push(line);

    bool ex = false;
    if (m_txPending.compare_exchange_strong(ex, true, std::memory_order_relaxed))
        sendSocketData();
}

void EthStratumClient::sendSocketData()
{
    if (!isConnected() || m_txQueue.empty())
    {
        m_sendBuffer.consume(m_sendBuffer.capacity());
        m_txQueue.consume_all([](std::string* l) { delete l; });
        m_txPending.store(false, std::memory_order_relaxed);
        return;
    }

    std::string* line;
    std::ostream os(&m_sendBuffer);
    while (m_txQueue.pop(line))
    {
        os << *line << std::endl;
        // Out received message only for debug purpouses
        if (g_logOptions & LOG_JSON)
            cnote << " >> " << *line;

        delete line;
    }

    if (m_conn->SecLevel() != SecureLevel::NONE)
    {
        async_write(*m_securesocket, m_sendBuffer,
            m_io_strand.wrap(boost::bind(&EthStratumClient::onSendSocketDataCompleted, this,
                boost::asio::placeholders::error)));
    }
    else
    {
        async_write(*m_nonsecuresocket, m_sendBuffer,
            m_io_strand.wrap(boost::bind(&EthStratumClient::onSendSocketDataCompleted, this,
                boost::asio::placeholders::error)));
    }
}

void EthStratumClient::onSendSocketDataCompleted(const boost::system::error_code& ec)
{
    if (ec)
    {
        m_sendBuffer.consume(m_sendBuffer.capacity());
        m_txQueue.consume_all([](std::string* l) { delete l; });
        m_txPending.store(false, std::memory_order_relaxed);

        if ((ec.category() == boost::asio::error::get_ssl_category()) &&
            (SSL_R_PROTOCOL_IS_SHUTDOWN == ERR_GET_REASON(ec.value())))
        {
            cnote << "SSL Stream error :" << ec.message();
            m_io_service.post(m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
        }

        if (isConnected())
        {
            cwarn << "Socket write failed: " + ec.message();
            m_io_service.post(m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect, this)));
        }
    }
    else
    {
        if (m_txQueue.empty())
            m_txPending.store(false, std::memory_order_relaxed);
        else
            sendSocketData();
    }
}

void EthStratumClient::onSSLShutdownCompleted(const boost::system::error_code& ec)
{
    (void)ec;
    clear_response_pleas();
    m_io_service.post(m_io_strand.wrap(boost::bind(&EthStratumClient::disconnect_finalize, this)));
}

void EthStratumClient::enqueue_response_plea()
{
    using namespace std::chrono;
    steady_clock::time_point response_plea_time = steady_clock::now();
    if (m_response_pleas_count++ == 0)
    {
        m_response_plea_older.store(
            response_plea_time.time_since_epoch(), std::memory_order_relaxed);
    }
    m_response_plea_times.push(response_plea_time);
}

std::chrono::milliseconds EthStratumClient::dequeue_response_plea()
{
    using namespace std::chrono;

    steady_clock::time_point response_plea_time(
        m_response_plea_older.load(std::memory_order_relaxed));
    milliseconds response_delay_ms =
        duration_cast<milliseconds>(steady_clock::now() - response_plea_time);

    if (m_response_plea_times.pop(response_plea_time))
    {
        m_response_plea_older.store(
            response_plea_time.time_since_epoch(), std::memory_order_relaxed);
    }
    if (m_response_pleas_count.load(std::memory_order_relaxed) > 0)
    {
        m_response_pleas_count--;
        return response_delay_ms;
    }
    else
    {
        return milliseconds(0);
    }
}

void EthStratumClient::clear_response_pleas()
{
    using namespace std::chrono;
    steady_clock::time_point response_plea_time;
    m_response_pleas_count.store(0, std::memory_order_relaxed);
    while (m_response_plea_times.pop(response_plea_time))
    {
    };
    m_response_plea_older.store(((steady_clock::time_point)steady_clock::now()).time_since_epoch(),
        std::memory_order_relaxed);
}
