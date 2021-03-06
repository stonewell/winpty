// Copyright (c) 2011-2015 Ryan Prichard
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

#include "ControlHandler.h"

#include <assert.h>
#include <errno.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include "../shared/DebugClient.h"
#include "Event.h"
#include "Util.h"
#include "WakeupFd.h"

ControlHandler::ControlHandler(HANDLE r, HANDLE w, WakeupFd &completionWakeup) :
    m_read_pipe(r),
    m_write_pipe(w),
    m_completionWakeup(completionWakeup),
    m_threadHasBeenJoined(false),
    m_shouldShutdown(0),
    m_threadCompleted(0)
{
    pthread_create(&m_thread, NULL, ControlHandler::threadProcS, this);
}

void ControlHandler::shutdown() {
    startShutdown();
    if (!m_threadHasBeenJoined) {
        int ret = pthread_join(m_thread, NULL);
        assert(ret == 0 && "pthread_join failed");
        m_threadHasBeenJoined = true;
    }
}

static BOOL read_pipe(HANDLE p, char * buf, int read_size) {
    char * tmp = buf;
    
    while (read_size > 0) {
        DWORD numRead = 0;
        BOOL ret = ReadFile(p,
                            tmp,
                            read_size,
                            &numRead,
                            NULL);

        if (!ret || numRead == 0) {
            return FALSE;
        }

        read_size -= numRead;
        tmp += numRead;
    }

    return TRUE;
}

static BOOL read_packet(HANDLE p, std::vector<char> & buf) {
    typedef unsigned __int64 uint64_t;
    
    uint64_t size = 0;

    char * tmp = (char *)&size;
    if (!read_pipe(p, tmp, sizeof(uint64_t)))
        return FALSE;

    buf.insert(buf.end(), tmp, tmp + sizeof(uint64_t));
    buf.resize(size);

    return read_pipe(p, &buf[sizeof(uint64_t)], size - sizeof(uint64_t));
}

void ControlHandler::threadProc() {
    while (true) {
        // Handle shutdown
        m_wakeup.reset();
        if (m_shouldShutdown) {
            trace("ControlHandler: shutting down");
            break;
        }

        // Read from the pipe.
        std::vector<char> data;

        ConnectNamedPipe(m_read_pipe, NULL);

        BOOL ret = read_packet(m_read_pipe,
                               data);
        
        if (!ret) {
            trace("ControlHandler: read failed: "
                  "ret=%d lastError=0x%x",
                  ret,
                  static_cast<unsigned int>(GetLastError()));
            break;
        }
        
        //Write to pipe
        DWORD written;
        ret = WriteFile(m_write_pipe,
                        &data[0], data.size(),
                        &written,
                        NULL);
        if (!ret || written != data.size()) {
            if (!ret && GetLastError() == ERROR_BROKEN_PIPE) {
                trace("ControlHandler: pipe closed: written=%u",
                      static_cast<unsigned int>(written));
            } else {
                trace("ControlHandler: write failed: "
                      "ret=%d lastError=0x%x numRead=%ld written=%u",
                      ret,
                      static_cast<unsigned int>(GetLastError()),
                      static_cast<DWORD>(data.size()),
                      static_cast<unsigned int>(written));
            }
            break;
        }

        DWORD numRead = 0;
        ReadFile(m_write_pipe, &data[0], 4, &numRead, NULL);
        WriteFile(m_read_pipe, &data[0], numRead, &written, NULL);
    }
    m_threadCompleted = 1;
    m_completionWakeup.set();
}
