/*
 * Authored by Alex Hultman, 2018-2020.
 * Intellectual property of third-party.

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef UWS_HTTPRESPONSE_H
#define UWS_HTTPRESPONSE_H

/* An HttpResponse is the channel on which you send back a response */

#include "AsyncSocket.h"
#include "HttpResponseData.h"
#include "HttpContextData.h"
#include "Utilities.h"

#include "WebSocketExtensions.h"
#include "WebSocketHandshake.h"
#include "WebSocket.h"
#include "WebSocketContextData.h"

#include "f2/function2.hpp"

/* todo: tryWrite is missing currently, only send smaller segments with write */

namespace uWS {

/* Some pre-defined status constants to use with writeStatus */
static const char *HTTP_200_OK = "200 OK";

/* The general timeout for HTTP sockets */
static const int HTTP_TIMEOUT_S = 10;

template <bool SSL>
struct HttpResponse : public AsyncSocket<SSL> {
    /* Solely used for getHttpResponseData() */
    template <bool> friend struct TemplatedApp;
    typedef AsyncSocket<SSL> Super;
private:
    HttpResponseData<SSL> *getHttpResponseData() {
        return (HttpResponseData<SSL> *) Super::getAsyncSocketData();
    }

    /* Write an unsigned 32-bit integer in hex */
    void writeUnsignedHex(unsigned int value) {
        char buf[10];
        int length = utils::u32toaHex(value, buf);

        /* For now we do this copy */
        Super::write(buf, length);
    }

    /* Write an unsigned 64-bit integer */
    void writeUnsigned64(uint64_t value) {
        char buf[20];
        int length = utils::u64toa(value, buf);

        /* For now we do this copy */
        Super::write(buf, length);
    }

    /* When we are done with a response we mark it like so */
    void markDone(HttpResponseData<SSL> *httpResponseData) {
        httpResponseData->onAborted = nullptr;
        /* Also remove onWritable so that we do not emit when draining behind the scenes. */
        httpResponseData->onWritable = nullptr;

        /* We are done with this request */
        httpResponseData->state &= ~HttpResponseData<SSL>::HTTP_RESPONSE_PENDING;
    }

    /* Called only once per request */
    void writeMark() {
        /* You can disable this altogether */
#ifndef UWS_HTTPRESPONSE_NO_WRITEMARK
        if (!Super::getLoopData()->noMark) {
            /* We only expose major version */
            writeHeader("uWebSockets", "18");
        }
#endif
    }

    /* Returns true on success, indicating that it might be feasible to write more data.
     * Will start timeout if stream reaches totalSize or write failure. */
    bool internalEnd(std::string_view data, size_t totalSize, bool optional, bool allowContentLength = true) {
        /* Write status if not already done */
        writeStatus(HTTP_200_OK);

        /* If no total size given then assume this chunk is everything */
        if (!totalSize) {
            totalSize = data.length();
        }

        HttpResponseData<SSL> *httpResponseData = getHttpResponseData();
        if (httpResponseData->state & HttpResponseData<SSL>::HTTP_WRITE_CALLED) {

            /* We do not have tryWrite-like functionalities, so ignore optional in this path */

            /* Do not allow sending 0 chunk here */
            if (data.length()) {
                Super::write("\r\n", 2);
                writeUnsignedHex((unsigned int) data.length());
                Super::write("\r\n", 2);

                /* Ignoring optional for now */
                Super::write(data.data(), (int) data.length());
            }

            /* Terminating 0 chunk */
            Super::write("\r\n0\r\n\r\n", 7);

            markDone(httpResponseData);

            /* tryEnd can never fail when in chunked mode, since we do not have tryWrite (yet), only write */
            Super::timeout(HTTP_TIMEOUT_S);
            return true;
        } else {
            /* Write content-length on first call */
            if (!(httpResponseData->state & HttpResponseData<SSL>::HTTP_END_CALLED)) {
                /* Write mark, this propagates to WebSockets too */
                writeMark();

                /* WebSocket upgrades does not allow content-length */
                if (allowContentLength) {
                    /* Even zero is a valid content-length */
                    Super::write("Content-Length: ", 16);
                    writeUnsigned64(totalSize);
                    Super::write("\r\n\r\n", 4);
                } else {
                    Super::write("\r\n", 2);
                }

                /* Mark end called */
                httpResponseData->state |= HttpResponseData<SSL>::HTTP_END_CALLED;
            }

            /* Even if we supply no new data to write, its failed boolean is useful to know
             * if it failed to drain any prior failed header writes */

            /* Write as much as possible without causing backpressure */
            size_t written = 0;
            bool failed = false;
            while (written < data.length() && !failed) {
                /* uSockets only deals with int sizes, so pass chunks of max signed int size */
                auto writtenFailed = Super::write(data.data() + written, (int) std::min<size_t>(data.length() - written, INT_MAX), optional);

                written += writtenFailed.first;
                failed = writtenFailed.second;
            }

            httpResponseData->offset += written;

            /* Success is when we wrote the entire thing without any failures */
            bool success = written == data.length() && !failed;

            /* If we are now at the end, start a timeout. Also start a timeout if we failed. */
            if (!success || httpResponseData->offset == totalSize) {
                Super::timeout(HTTP_TIMEOUT_S);
            }

            /* Remove onAborted function if we reach the end */
            if (httpResponseData->offset == totalSize) {
                markDone(httpResponseData);
            }

            return success;
        }
    }

public:
    /* If we have proxy support; returns the proxed source address as reported by the proxy. */
#ifdef UWS_WITH_PROXY
    std::string_view getProxiedRemoteAddress() {
        return getHttpResponseData()->proxyParser.getSourceAddress();
    }

    std::string_view getProxiedRemoteAddressAsText() {
        return Super::addressAsText(getProxiedRemoteAddress());
    }
#endif

    /* Manually upgrade to WebSocket. Typically called in upgrade handler. Immediately calls open handler.
     * NOTE: Will invalidate 'this' as socket might change location in memory. Throw away aftert use. */
    template <typename UserData>
    void upgrade(UserData &&userData, std::string_view secWebSocketKey, std::string_view secWebSocketProtocol,
            std::string_view secWebSocketExtensions,
            struct us_socket_context_t *webSocketContext) {

        /* Extract needed parameters from WebSocketContextData */
        WebSocketContextData<SSL> *webSocketContextData = (WebSocketContextData<SSL> *) us_socket_context_ext(SSL, webSocketContext);

        /* Note: OpenSSL can be used here to speed this up somewhat */
        char secWebSocketAccept[29] = {};
        WebSocketHandshake::generate(secWebSocketKey.data(), secWebSocketAccept);

        writeStatus("101 Switching Protocols")
            ->writeHeader("Upgrade", "websocket")
            ->writeHeader("Connection", "Upgrade")
            ->writeHeader("Sec-WebSocket-Accept", secWebSocketAccept);

        /* Select first subprotocol if present */
        if (secWebSocketProtocol.length()) {
            writeHeader("Sec-WebSocket-Protocol", secWebSocketProtocol.substr(0, secWebSocketProtocol.find(',')));
        }

        /* Negotiate compression, we may use a smaller compression window than we negotiate */
        bool perMessageDeflate = false;
        /* We are always allowed to share compressor, if perMessageDeflate */
        int compressOptions = webSocketContextData->compression & SHARED_COMPRESSOR;
        if (webSocketContextData->compression != DISABLED) {
            if (secWebSocketExtensions.length()) {
                /* We never support client context takeover (the client cannot compress with a sliding window). */
                int wantedOptions = PERMESSAGE_DEFLATE | CLIENT_NO_CONTEXT_TAKEOVER;

                /* Shared compressor is the default */
                if (webSocketContextData->compression == SHARED_COMPRESSOR) {
                    /* Disable per-socket compressor */
                    wantedOptions |= SERVER_NO_CONTEXT_TAKEOVER;
                }

                /* isServer = true */
                ExtensionsNegotiator<true> extensionsNegotiator(wantedOptions);
                extensionsNegotiator.readOffer(secWebSocketExtensions);

                /* Todo: remove these mid string copies */
                std::string offer = extensionsNegotiator.generateOffer();
                if (offer.length()) {

                    /* Todo: this is a quick fix that should be properly moved to ExtensionsNegotiator */
                    if (webSocketContextData->compression & DEDICATED_COMPRESSOR &&
                        webSocketContextData->compression != DEDICATED_COMPRESSOR_256KB) {
                            /* 3kb, 4kb is 9, 256 is 15 (default) */
                            int maxServerWindowBits = 9;
                            switch (webSocketContextData->compression) {
                                case DEDICATED_COMPRESSOR_8KB:
                                maxServerWindowBits = 10;
                                break;
                                case DEDICATED_COMPRESSOR_16KB:
                                maxServerWindowBits = 11;
                                break;
                                case DEDICATED_COMPRESSOR_32KB:
                                maxServerWindowBits = 12;
                                break;
                                case DEDICATED_COMPRESSOR_64KB:
                                maxServerWindowBits = 13;
                                break;
                                case DEDICATED_COMPRESSOR_128KB:
                                maxServerWindowBits = 14;
                                break;
                            }
                            offer += "; server_max_window_bits=";
                            offer += std::to_string(maxServerWindowBits);
                    }

                    writeHeader("Sec-WebSocket-Extensions", offer);
                }

                /* Did we negotiate permessage-deflate? */
                if (extensionsNegotiator.getNegotiatedOptions() & PERMESSAGE_DEFLATE) {
                    perMessageDeflate = true;
                }

                /* Is the server allowed to compress with a sliding window? */
                if (!(extensionsNegotiator.getNegotiatedOptions() & SERVER_NO_CONTEXT_TAKEOVER)) {
                    compressOptions = webSocketContextData->compression;
                }
            }
        }

        internalEnd({nullptr, 0}, 0, false, false);

        /* Grab the httpContext from res */
        HttpContext<SSL> *httpContext = (HttpContext<SSL> *) us_socket_context(SSL, (struct us_socket_t *) this);

        /* Move any backpressure out of HttpResponse */
        std::string backpressure(std::move(((AsyncSocketData<SSL> *) getHttpResponseData())->buffer));

        /* Destroy HttpResponseData */
        getHttpResponseData()->~HttpResponseData();

        /* Before we adopt and potentially change socket, check if we are corked */
        bool wasCorked = Super::isCorked();

        /* Adopting a socket invalidates it, do not rely on it directly to carry any data */
        WebSocket<SSL, true> *webSocket = (WebSocket<SSL, true> *) us_socket_context_adopt_socket(SSL,
                    (us_socket_context_t *) webSocketContext, (us_socket_t *) this, sizeof(WebSocketData) + sizeof(UserData));

        /* For whatever reason we were corked, update cork to the new socket */
        if (wasCorked) {
            webSocket->AsyncSocket<SSL>::cork();
        }

        /* Initialize websocket with any moved backpressure intact */
        webSocket->init(perMessageDeflate, compressOptions, std::move(backpressure));

        /* We should only mark this if inside the parser; if upgrading "async" we cannot set this */
        HttpContextData<SSL> *httpContextData = httpContext->getSocketContextData();
        if (httpContextData->isParsingHttp) {
            /* We need to tell the Http parser that we changed socket */
            httpContextData->upgradedWebSocket = webSocket;
        }

        /* Arm idleTimeout */
        us_socket_timeout(SSL, (us_socket_t *) webSocket, webSocketContextData->idleTimeout);

        /* Move construct the UserData right before calling open handler */
        new (webSocket->getUserData()) UserData(std::move(userData));

        /* Emit open event and start the timeout */
        if (webSocketContextData->openHandler) {
            webSocketContextData->openHandler(webSocket);
        }
    }

    /* Immediately terminate this Http response */
    using Super::close;

    /* See AsyncSocket */
    using Super::getRemoteAddress;
    using Super::getRemoteAddressAsText;
    using Super::getNativeHandle;

    /* Note: Headers are not checked in regards to timeout.
     * We only check when you actively push data or end the request */

    /* Write 100 Continue, can be done any amount of times */
    HttpResponse *writeContinue() {
        Super::write("HTTP/1.1 100 Continue\r\n\r\n", 25);
        return this;
    }

    /* Write the HTTP status */
    HttpResponse *writeStatus(std::string_view status) {
        HttpResponseData<SSL> *httpResponseData = getHttpResponseData();

        /* Do not allow writing more than one status */
        if (httpResponseData->state & HttpResponseData<SSL>::HTTP_STATUS_CALLED) {
            return this;
        }

        /* Update status */
        httpResponseData->state |= HttpResponseData<SSL>::HTTP_STATUS_CALLED;

        Super::write("HTTP/1.1 ", 9);
        Super::write(status.data(), (int) status.length());
        Super::write("\r\n", 2);
        return this;
    }

    /* Write an HTTP header with string value */
    HttpResponse *writeHeader(std::string_view key, std::string_view value) {
        writeStatus(HTTP_200_OK);

        Super::write(key.data(), (int) key.length());
        Super::write(": ", 2);
        Super::write(value.data(), (int) value.length());
        Super::write("\r\n", 2);
        return this;
    }

    /* Write an HTTP header with unsigned int value */
    HttpResponse *writeHeader(std::string_view key, uint64_t value) {
        Super::write(key.data(), (int) key.length());
        Super::write(": ", 2);
        writeUnsigned64(value);
        Super::write("\r\n", 2);
        return this;
    }

    /* End the response with an optional data chunk. Always starts a timeout. */
    void end(std::string_view data = {}) {
        internalEnd(data, data.length(), false);
    }

    /* Try and end the response. Returns [true, true] on success.
     * Starts a timeout in some cases. Returns [ok, hasResponded] */
    std::pair<bool, bool> tryEnd(std::string_view data, size_t totalSize = 0) {
        return {internalEnd(data, totalSize, true), hasResponded()};
    }

    /* Write parts of the response in chunking fashion. Starts timeout if failed. */
    bool write(std::string_view data) {
        writeStatus(HTTP_200_OK);

        /* Do not allow sending 0 chunks, they mark end of response */
        if (!data.length()) {
            /* If you called us, then according to you it was fine to call us so it's fine to still call us */
            return true;
        }

        HttpResponseData<SSL> *httpResponseData = getHttpResponseData();

        if (!(httpResponseData->state & HttpResponseData<SSL>::HTTP_WRITE_CALLED)) {
            /* Write mark on first call to write */
            writeMark();

            writeHeader("Transfer-Encoding", "chunked");
            httpResponseData->state |= HttpResponseData<SSL>::HTTP_WRITE_CALLED;
        }

        Super::write("\r\n", 2);
        writeUnsignedHex((unsigned int) data.length());
        Super::write("\r\n", 2);

        auto [written, failed] = Super::write(data.data(), (int) data.length());
        if (failed) {
            Super::timeout(HTTP_TIMEOUT_S);
        }

        /* If we did not fail the write, accept more */
        return !failed;
    }

    /* Get the current byte write offset for this Http response */
    size_t getWriteOffset() {
        HttpResponseData<SSL> *httpResponseData = getHttpResponseData();

        return httpResponseData->offset;
    }

    /* Checking if we have fully responded and are ready for another request */
    bool hasResponded() {
        HttpResponseData<SSL> *httpResponseData = getHttpResponseData();

        return !(httpResponseData->state & HttpResponseData<SSL>::HTTP_RESPONSE_PENDING);
    }

    /* Corks the response if possible. Leaves already corked socket be. */
    HttpResponse *cork(fu2::unique_function<void()> &&handler) {
        if (!Super::isCorked() && Super::canCork()) {
            Super::cork();
            handler();

            /* Timeout on uncork failure, since most writes will succeed while corked */
            auto [written, failed] = Super::uncork();
            if (failed) {
                /* For now we only have one single timeout so let's use it */
                /* This behavior should equal the behavior in HttpContext when uncorking fails */
                Super::timeout(HTTP_TIMEOUT_S);
            }
        } else {
            /* We are already corked, or can't cork so let's just call the handler */
            handler();
        }

        return this;
    }

    /* Attach handler for writable HTTP response */
    HttpResponse *onWritable(fu2::unique_function<bool(size_t)> &&handler) {
        HttpResponseData<SSL> *httpResponseData = getHttpResponseData();

        httpResponseData->onWritable = std::move(handler);
        return this;
    }

    /* Attach handler for aborted HTTP request */
    HttpResponse *onAborted(fu2::unique_function<void()> &&handler) {
        HttpResponseData<SSL> *httpResponseData = getHttpResponseData();

        httpResponseData->onAborted = std::move(handler);
        return this;
    }

    /* Attach a read handler for data sent. Will be called with FIN set true if last segment. */
    void onData(fu2::unique_function<void(std::string_view, bool)> &&handler) {
        HttpResponseData<SSL> *data = getHttpResponseData();
        data->inStream = std::move(handler);
    }
};

}

#endif // UWS_HTTPRESPONSE_H
