// util/kaldi-curlbuf.cc

// Copyright 2009-2011  LINSE/UFSC (author: Augusto Henrique Hentz)

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//  http://www.apache.org/licenses/LICENSE-2.0

// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#ifdef HAVE_LIBCURL

#include "util/kaldi-curlbuf.h"
#include "base/timer.h"
#include "base/kaldi-common.h"

#include <algorithm>
#include <cstring>
#include <cerrno>
#include <iostream>

using std::size_t;
using std::cerr;

namespace kaldi {

curlbuf::curlbuf(size_t put_back, int timeout) :
    put_back_(std::max(put_back, size_t(1))),
    timeout_(timeout),
    url_(""),
    curl_(NULL),
    multi_(NULL),
    buffer_(std::max(size_t(CURL_MAX_WRITE_SIZE), put_back_) + put_back_),
    running_(0),
    status_code_(0)
{
    setg(NULL, NULL, NULL);
}

int curlbuf::status_code() const
{
    return status_code_;
}

int curlbuf::timeout() const
{
    return timeout_;
}

void curlbuf::set_timeout(int timeout)
{
    timeout_ = timeout;
}

bool curlbuf::open(std::string url)
{
    url_ = url;
    status_code_ = 0;
    running_ = 0;
    setg(NULL, NULL, NULL);

    curl_ = curl_easy_init();
    multi_ = curl_multi_init();

    if (!curl_ || !multi_) return false;

    curl_easy_setopt(curl_, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_, CURLOPT_VERBOSE, 0L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, curlbuf::curl_callback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, static_cast<void*>(this));
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, curlbuf::header_callback);
    curl_easy_setopt(curl_, CURLOPT_WRITEHEADER, static_cast<void*>(this));
    curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, error_buffer_);

    curl_multi_add_handle(multi_, curl_);

    KALDI_ASSERT(eback() == NULL && gptr() == NULL && egptr() == NULL);
    if (curl_multi_perform(multi_, &running_) != CURLM_OK) {
        //KALDI_WARN << "Error in curl_multi_perform (1) " << url_;
        close();
        return false;
    }

    if (running_) {
        if (!status_code_ || eback() == NULL) {
            // Calling fill_buffer() to (at least) fetch the HTTP status code in case nothing
            // came during the first curl_multi_perform() call.
            fill_buffer();
	}
    }
    else {
        //KALDI_WARN << "CURL not running";
	struct CURLMsg* m = NULL;
	do {
	    int msgq = 0;
            //KALDI_WARN << "Antes de curl_multi_info_read";
	    m = curl_multi_info_read(multi_, &msgq);
   
            if (m && (m->msg == CURLMSG_DONE)) {
	        //KALDI_WARN << "CURL message " << m << "->msg = " << m->msg << ". " << msgq << " messages left in queue";
		KALDI_ASSERT(m->easy_handle == curl_);
		if (m->data.result != CURLE_OK) {
		    KALDI_WARN << "Error in transfer. Return code is " << m->data.result << ": " << error_buffer_;
		    close();
		    return false;
		}
		get_status_code();
                //KALDI_WARN << "Status code is " << status_code_;
	    } 
	} while(m); 
        //KALDI_WARN << "Todas as mensagens lidas";
        fill_buffer();
        //KALDI_WARN << "Depois do fill_buffer()";
    }


    if (!status_code_) {
        KALDI_WARN << "Error opening " << url_ << ". Status code = " << status_code_ << ": " << error_buffer_;
        close();
        return false;
    }
    else if (status_code_ / 100 == 4 || status_code_ / 100 == 5) {
        KALDI_WARN << "Error opening " << url_ << " (HTTP status code " << status_code_ << ")" << ": " << error_buffer_;
        close();
        return false;
    }
    else if (eback() == NULL || gptr() == NULL || egptr() == NULL || gptr() != eback()) {
        KALDI_WARN << "We have a valid status code, but no body" << ": " << error_buffer_;
        close();
        return false;
    }
    //KALDI_WARN << "Return true";
    return true;
}

void curlbuf::close()
{
    if (multi_) {
        if (curl_) {
            curl_multi_remove_handle(multi_, curl_);
            curl_easy_cleanup(curl_);
            curl_ = NULL;
        }
        curl_multi_cleanup(multi_);
        multi_ = NULL;
    }
}

curlbuf::~curlbuf()
{
    close();
}

std::streambuf::int_type curlbuf::underflow()
{
    if (gptr() < egptr()) // buffer not exhausted
        return traits_type::to_int_type(*gptr());

    return fill_buffer() ? traits_type::to_int_type(*gptr()) : traits_type::eof();
}

bool curlbuf::fill_buffer() {
    Timer t;

    // Now we have to call libcurl to fill the buffer
    do {
        fd_set fdread;
        fd_set fdwrite;
        fd_set fdexcep;
        struct timeval timeout;
        int maxfd = -1;
        long curl_timeo = -1;
        int rc;

        FD_ZERO(&fdread);
        FD_ZERO(&fdwrite);
        FD_ZERO(&fdexcep);

        /* set a suitable timeout to fail on */ 
        curl_multi_timeout(multi_, &curl_timeo);
        if(curl_timeo >= 0) {
            timeout.tv_sec  = std::max(curl_timeo / 1000, 1l);
            timeout.tv_usec = (curl_timeo % 1000) * 1000;
        }
        else {
            timeout.tv_sec  = timeout_;
            timeout.tv_usec = 0;
        }

        curl_multi_fdset(multi_, &fdread, &fdwrite, &fdexcep, &maxfd);
        rc = select(maxfd+1, &fdread, &fdwrite, &fdexcep, &timeout);
        if (rc == -1) {
            KALDI_WARN << "curlbuf error: [Errno " << errno << "] " << strerror(errno);
            return false;
        }

        if (rc == 0 && t.Elapsed() >= timeout_) {
            KALDI_WARN << "curlbuf error: timeout" << std::endl;
            return false;
        }

        curl_multi_perform(multi_, &running_);
    } while (running_ && gptr() >= egptr());

    if (!running_ && gptr() == egptr()) {
        KALDI_WARN << "Erro aqui! gptr=" << (void*)gptr() << ", egptr=" << (void*)egptr();
        return false;
    }

    return true;
}

size_t
curlbuf::curl_callback(char *buffer, size_t size, size_t nitems, void *userp)
{
    curlbuf *This = static_cast<curlbuf*>(userp);
    size *= nitems;

    if (size > This->buffer_.size() - This->put_back_) {
        cerr << "curlbuf warning: resizing internal buffer (this is harmless, but should not happen)" << std::endl;
        This->buffer_.resize(size + This->put_back_);
    }

    char *base = &This->buffer_.front();
    char *start = base;

    if (This->eback() != NULL) // true when this isn't the first fill
    {
        size_t pback = std::min(This->put_back_, size_t(This->gptr() - This->eback()));
        // Move `put_back_` bytes from the end of the buffer to the
        // put back area
        std::memmove(base, This->egptr() - pback, pback);
        start += pback;
    }

    std::memcpy(start, buffer, size);
    This->setg(base, start, start + size);

    return size;
}

void curlbuf::get_status_code()
{
    if (status_code_ == 0) {
    	long statuscode = 0l;
    	CURLcode cc = curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &statuscode);
	if (cc != CURLE_OK) {
	    KALDI_ERR << "Error in curl_easy_getinfo(CURLINFO_RESPONSE_CODE)";
	    return;
	}
	status_code_ = statuscode;
    }
}

size_t
curlbuf::header_callback(char *buffer, size_t size, size_t nitems, void *userp)
{
    curlbuf *This = static_cast<curlbuf*>(userp);
    size *= nitems;
    This->get_status_code();





#if 0
    if (strncmp(buffer, "HTTP/", sizeof("HTTP/")-1) == 0) {
        char code[4]; // According to RFC2616, HTTP status codes must be 3 digits long
        char *begin = buffer;
        bool space = false;
        while (++begin < buffer + size - 3) {
            if (*begin == ' ') space = true;
            if (space && *begin != ' ') break;
        }
        code[0] = begin[0];
        code[1] = begin[1];
        code[2] = begin[2];
        code[3] = '\0';
        This->status_code_ = atoi(code);

        // Abort the connection right now in case of error
        if (This->status_code_ == 0 || code[0] == '4' || code[0] == '5') return 0;
    }
#endif

    return size;
}

}; // end namespace kaldi

#endif //HAVE_LIBCURL
