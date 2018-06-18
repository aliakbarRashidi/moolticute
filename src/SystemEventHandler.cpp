#include "SystemEventHandler.h"

#ifdef Q_OS_MAC
#include "MacSystemEvents.h"
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <wtsapi32.h>
#endif

#include <QCoreApplication>
#include <QDebug>

SystemEventHandler::SystemEventHandler()
#ifdef Q_OS_WIN
    : wtsApi32Lib("wtsapi32")
#endif
{
#ifdef Q_OS_MAC
    Q_ASSERT(!eventHandler);
    eventHandler = registerSystemHandler(this, &SystemEventHandler::triggerEvent);
#elif defined(Q_OS_WIN)
    qApp->installNativeEventFilter(this);

    if (wtsApi32Lib.load())
    {
        typedef WINBOOL (*RegFunc)(HWND, DWORD);
        const auto regFunc = (RegFunc) wtsApi32Lib.resolve("WTSRegisterSessionNotification");
        if (regFunc) {
            regFunc((HWND) widget.winId(), 0);
        }
    }
#endif
}

SystemEventHandler::~SystemEventHandler()
{
#ifdef Q_OS_MAC
    Q_ASSERT(eventHandler);
    unregisterSystemHandler(eventHandler);
#elif defined(Q_OS_WIN)
    qApp->removeNativeEventFilter(this);

    if (wtsApi32Lib.isLoaded())
    {
        typedef WINBOOL (*UnRegFunc)(HWND);
        const auto unRegFunc = (UnRegFunc) wtsApi32Lib.resolve("WTSUnRegisterSessionNotification");
        if (unRegFunc) {
            unRegFunc((HWND) widget.winId());
        }
    }
#endif
}

void SystemEventHandler::emitEvent(const SystemEvent event)
{
    switch (event)
    {
        case SCREEN_LOCKED:
            emit screenLocked();
            break;

        case LOGGING_OFF:
            emit loggingOff();
            break;

        case GOING_TO_SLEEP:
            emit goingToSleep();
            break;

       case SHUTTING_DOWN:
            emit shuttingDown();
            break;

        default:
            qCritical() << "Unknown system event:" << event;
            Q_ASSERT(false);
            break;
    }
}

void SystemEventHandler::triggerEvent(const int type, void *instance)
{
    auto *handler = static_cast<SystemEventHandler*>(instance);
    if (handler) {
        handler->emitEvent(static_cast<SystemEvent>(type));
    }
}

bool SystemEventHandler::nativeEventFilter(const QByteArray &eventType, void *message, long *result)
{
    Q_UNUSED(result);

#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG")
    {
        const auto *msg = (MSG*) message;
        if (msg->message == WM_ENDSESSION || msg->message == WM_QUERYENDSESSION)
        {
            if (msg->lParam == static_cast<int>(ENDSESSION_LOGOFF))
            {
                emit loggingOff();
            }
            else
            {
                emit shuttingDown();
            }
        }
        else if (msg->message == WM_POWERBROADCAST && msg->wParam == PBT_APMSUSPEND)
        {
            emit goingToSleep();
        }
        else if (msg->message == WM_WTSSESSION_CHANGE && msg->wParam == WTS_SESSION_LOCK)
        {
            emit screenLocked();
        }
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
#endif

  return false;
}

#ifdef Q_OS_MAC
void SystemEventHandler::readyToTerminate()
{
    ::readyToTerminate();
}
#endif
