/*
 * Copyright (C) 2022 Sky UK
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <errno.h>
#include <gst/gst.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>

#include "Timer.h"

/**
 * @brief The timer thread static function.
 *
 * @param[in] self : A void 'this' pointer.
 *
 * @retval not NULL if exit without error.
 */
void *Timer::timerThreadFunction(void *vSelf)
{
    GST_DEBUG("entry: vSelf=%p", vSelf);

    Timer *self = static_cast<Timer *>(vSelf);

    return (void *)self->timerThread();
}

/**
 * @brief Default constructor.
 *
 * @param[in] callbackFunction : The callback function to call on expiry.
 * @param[in] userData         : Opaque data to return via the callback function.
 */
Timer::Timer(TimerCallback callbackFunction, void *userData, const char *name)
    : m_callbackFunction(callbackFunction), m_userData(userData), m_name(name)
{
    GST_DEBUG("entry: Self=%p callbackFunction=%p,userData=%p name=%s", this, callbackFunction, userData, name);

    int ret = pthread_mutex_init(&m_mutex, NULL);
    if (ret)
    {
        GST_ERROR("Failed to create timer mutex: %s", strerror(ret));
        throw TimerException("Failed to create timer mutex");
    }

    ret = pthread_condattr_init(&m_condAttr);
    if (ret)
    {
        GST_ERROR("Failed to create condition variable attributes: %s", strerror(ret));
        throw TimerException("Failed to create condition variable attributes");
    }

    // The condition variable clock is set to be a monotonic increasing
    // clock. I.e. time is linear and unique.
    ret = pthread_condattr_setclock(&m_condAttr, CLOCK_MONOTONIC);
    if (ret)
    {
        GST_ERROR("Failed to set condition variable attributes: %s", strerror(ret));
        throw TimerException("Failed to set condition variable attributes");
    }

    ret = pthread_cond_init(&m_cond, &m_condAttr);
    if (ret)
    {
        GST_ERROR("Failed to create condition variable: %s", strerror(ret));
        throw TimerException("Failed to create condition variable");
    }

    ret = pthread_create(&m_timerThread, NULL, timerThreadFunction, this);
    if (ret)
    {
        GST_ERROR("Failed to create timer tast: %s", strerror(ret));
        throw TimerException("Failed to create timer task");
    }
}

/**
 * @brief virtual destructor
 */
Timer::~Timer()
{
    GST_DEBUG("entry Self=%p", this);

    postTimerMessage(TimerMessage::createQuitMessage(), true);
    pthread_join(m_timerThread, NULL);
    pthread_cond_destroy(&m_cond);
    pthread_condattr_destroy(&m_condAttr);
    pthread_mutex_destroy(&m_mutex);
}

/**
 * @brief Cancels the timer.
 *
 * This method cancels any currently armed timer. The callback
 * function will not be called at any point after this method is
 * called.
 */
void Timer::cancel()
{
    GST_DEBUG("entry Self=%p", this);

    postTimerMessage(TimerMessage::createCancelMessage());
}

/**
 * @brief Arms the timer.
 *
 * This method arms the timer. After the specified period the
 * registered callback function will be called with the opaque user
 * data provided in the timer's constructor.
 *
 * This method MAY be called within the callback function. It is
 * legal to re-arm a timer without cancelling it.
 *
 * @param[in] timeout : the wait time in ms
 */
void Timer::arm(long timeout)
{
    GST_DEBUG("entry: Self=%p timeout=%ld", this, timeout);

    postTimerMessage(TimerMessage::createArmMessage(timeout));
}

/**
 * @brief The timer thread function.
 *
 * @retval true if exit without error.
 */
bool Timer::timerThread()
{
    // The timer thread function is a thread that pends on the message
    // queue condition variable. The pend will end for one of three
    // reasons. Either the time of the timer expiry has been reached, or
    // the thread was interrupted for some reason, or a new message was
    // sent. If the timer is not armed the condition variable will wait
    // for ever.
    //
    // If the condition variable times out the registered callback
    // function is called and the timer disarms.
    //
    // If the thread is interrupted the condition variable is re-entered.
    //
    // If a message is received the message is read from the queue and
    // handled and if it was neither a CANCEL nor QUIT message the timer
    // remains armed and the condition variable waits for the timeout
    // period. Cancelling the timer will disarm it. The quit message is
    // for when the timer is destroyed.

    prctl(PR_SET_NAME, m_name.c_str(), NULL, NULL, NULL);

    GST_DEBUG("entry Self=%p", this);

    struct timespec triggerTime;

    bool armed = false;
    bool quit = false;

    do
    {
        TimerMessage *message = 0;
        int ret = 0;

        pthread_mutex_lock(&m_mutex);

        while (m_msgQueue.empty() && 0 == ret)
        {
            if (!armed)
            {
                ret = pthread_cond_wait(&m_cond, &m_mutex);
                if (ret)
                {
                    GST_ERROR("pthread_cond_wait() failed: %s", strerror(ret));
                }
            }
            else
            {
                // trigger time is absolute
                ret = pthread_cond_timedwait(&m_cond, &m_mutex, &triggerTime);
                if (ETIMEDOUT == ret)
                {
                    GST_INFO("Timed out! Self=%p", this);
                }
                else if (EINTR == ret)
                {
                    GST_WARNING("Interrupted");

                    // loop and wait again
                    ret = 0;
                }
                else if (ret)
                {
                    GST_ERROR("pthread_cond_wait() failed: %s", strerror(errno));
                }
            }
        }

        if (ret == ETIMEDOUT)
        {
            pthread_mutex_unlock(&m_mutex);
            // Just incase we're not armed any more.
            if (armed)
            {
                GST_INFO("Timer expired! Self=%p", this);

                if (m_callbackFunction)
                {
                    m_callbackFunction(m_userData);
                }
            }

            armed = false;
        }
        else if (ret == 0)
        {
            message = m_msgQueue.front();
            m_msgQueue.pop_front();

            pthread_mutex_unlock(&m_mutex);

            if (message->getType() == TimerMessage::TIMER_MSG_ARM)
            {
                GST_INFO("TimerMessage::TIMER_MSG_ARM Self=%p", this);

                ret = clock_gettime(CLOCK_MONOTONIC, &triggerTime);
                if (ret)
                {
                    GST_ERROR("clock_gettime() failed: %s", strerror(errno));
                }
                else
                {
                    triggerTime.tv_sec += (message->getTimeout() / 1000);
                    triggerTime.tv_nsec += (message->getTimeout() % 1000) * 1000000;
                    triggerTime.tv_sec += (triggerTime.tv_nsec / 1000000000);
                    triggerTime.tv_nsec %= 1000000000;

                    armed = true;
                }
            }
            else if (message->getType() == TimerMessage::TIMER_MSG_CANCEL)
            {
                GST_INFO("TimerMessage::TIMER_MSG_CANCEL Self=%p", this);

                armed = false;
            }
            else if (message->getType() == TimerMessage::TIMER_MSG_QUIT)
            {
                GST_INFO("TimerMessage::TIMER_MSG_QUIT Self=%p", this);

                quit = true;
            }
            else
            {
                GST_WARNING("Unrecognised message: %d", (int)message->getType());
            }

            delete message;
        }
        else
        {
            // unhandled error
            goto error;
        }
    } while (!quit);

    GST_INFO("return true Self=%p", this);
    return true;

error:
    pthread_mutex_unlock(&m_mutex);

    GST_INFO("return false Self=%p", this);
    return false;
}

/**
 * @brief Posts a timer message to the timer thread.
 *
 * This method posts a timer message to the timer thread. Set urgent to
 * true to post a message to the front of the queue. I.e. ensure it is
 * the next message handled.
 *
 * @param[in] message : The timer message.
 * @param[in] urgent  : The urgency flag
 */
void Timer::postTimerMessage(TimerMessage *message, bool urgent)
{
    pthread_mutex_lock(&m_mutex);

    if (urgent)
    {
        m_msgQueue.push_front(message);
    }
    else
    {
        m_msgQueue.push_back(message);
    }

    pthread_cond_signal(&m_cond);
    pthread_mutex_unlock(&m_mutex);
}
