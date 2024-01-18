// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WindowThread.h"

using namespace winrt::Microsoft::Terminal::Remoting;

WindowThread::WindowThread(winrt::TerminalApp::AppLogic logic, WindowManager manager) :
    _appLogic{ std::move(logic) },
    _manager{ std::move(manager) }
{
    // DO NOT start the AppHost here in the ctor, as that will start XAML on the wrong thread!
}

void WindowThread::CreateHost(WindowRequestedArgs args)
{
    // Start the AppHost HERE, on the actual thread we want XAML to run on
    _peasant = _manager.CreatePeasant(args);
    _host = std::make_shared<::AppHost>(_appLogic, std::move(args), _manager, _peasant);

    _UpdateSettingsRequestedToken = _host->UpdateSettingsRequested([this]() { _UpdateSettingsRequestedHandlers(); });

    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Initialize the xaml content. This must be called AFTER the
    // WindowsXamlManager is initialized.
    _host->Initialize();

    _dispatcher = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
}

int WindowThread::RunMessagePump()
{
    // Enter the main window loop.
    const auto exitCode = _messagePump();
    // Here, the main window loop has exited.
    return exitCode;
}

void WindowThread::_pumpRemainingXamlMessages()
{
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        ::DispatchMessageW(&msg);
    }
}

void WindowThread::RundownForExit()
{
    if (_host)
    {
        _host->UpdateSettingsRequested(_UpdateSettingsRequestedToken);
        _host->Close();
    }
    if (_warmWindow)
    {
        // If we have a _warmWindow, we're a refrigerated thread without a
        // AppHost in control of the window. Manually close the window
        // ourselves, to free the DesktopWindowXamlSource.
        _warmWindow->Close();
    }

    // !! LOAD BEARING !!
    //
    // Make sure to finish pumping all the messages for our thread here. We
    // may think we're all done, but we're not quite. XAML needs more time
    // to pump the remaining events through, even at the point we're
    // exiting. So do that now. If you don't, then the last tab to close
    // will never actually destruct the last tab / TermControl / ControlCore
    // / renderer.
    _pumpRemainingXamlMessages();
}

// Method Description:
// - "Refrigerate" this thread for later reuse. This will refrigerate the window
//   itself, and tear down our current app host. We'll save our window for
//   later. We'll also pump out the existing message from XAML, before
//   returning. After we return, the emperor will add us to the list of threads
//   that can be re-used.
void WindowThread::Refrigerate()
{
    _host->UpdateSettingsRequested(_UpdateSettingsRequestedToken);
    // keep a reference to the HWND and DesktopWindowXamlSource alive.
    _warmWindow = std::move(_host->Refrigerate());
    _host = nullptr;
}

// Method Description:
// - "Reheat" this thread for reuse. We'll build a new AppHost, and pass in the
//   existing window to it. We'll then trigger the _microwaveBuzzer, so KeepWarm
//   (which is on the UI thread) will get unblocked, and we can initialize this
//   window.
void WindowThread::Microwave(WindowRequestedArgs args)
{
    _dispatcher.TryEnqueue(winrt::Windows::System::DispatcherQueuePriority::Normal, [this, args = std::move(args)]() {
        _peasant = _manager.CreatePeasant(args);
        _host = std::make_shared<::AppHost>(_appLogic, std::move(args), _manager, _peasant, std::move(_warmWindow));
        _UpdateSettingsRequestedToken = _host->UpdateSettingsRequested([this]() { _UpdateSettingsRequestedHandlers(); });
        _host->Initialize();
    });
}

winrt::TerminalApp::TerminalWindow WindowThread::Logic()
{
    return _host->Logic();
}

static bool _messageIsF7Keypress(const MSG& message)
{
    return (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && message.wParam == VK_F7;
}
static bool _messageIsAltKeyup(const MSG& message)
{
    return (message.message == WM_KEYUP || message.message == WM_SYSKEYUP) && message.wParam == VK_MENU;
}
static bool _messageIsAltSpaceKeypress(const MSG& message)
{
    return message.message == WM_SYSKEYDOWN && message.wParam == VK_SPACE;
}

int WindowThread::_messagePump()
{
    MSG message{};

    while (GetMessageW(&message, nullptr, 0, 0))
    {
        // GH#638 (Pressing F7 brings up both the history AND a caret browsing message)
        // The Xaml input stack doesn't allow an application to suppress the "caret browsing"
        // dialog experience triggered when you press F7. Official recommendation from the Xaml
        // team is to catch F7 before we hand it off.
        // AppLogic contains an ad-hoc implementation of event bubbling for a runtime classes
        // implementing a custom IF7Listener interface.
        // If the recipient of IF7Listener::OnF7Pressed suggests that the F7 press has, in fact,
        // been handled we can discard the message before we even translate it.
        if (_messageIsF7Keypress(message))
        {
            if (_host->OnDirectKeyEvent(VK_F7, LOBYTE(HIWORD(message.lParam)), true))
            {
                // The application consumed the F7. Don't let Xaml get it.
                continue;
            }
        }

        // GH#6421 - System XAML will never send an Alt KeyUp event. So, similar
        // to how we'll steal the F7 KeyDown above, we'll steal the Alt KeyUp
        // here, and plumb it through.
        if (_messageIsAltKeyup(message))
        {
            // Let's pass <Alt> to the application
            if (_host->OnDirectKeyEvent(VK_MENU, LOBYTE(HIWORD(message.lParam)), false))
            {
                // The application consumed the Alt. Don't let Xaml get it.
                continue;
            }
        }

        // GH#7125 = System XAML will show a system dialog on Alt Space. We want to
        // explicitly prevent that because we handle that ourselves. So similar to
        // above, we steal the event and hand it off to the host.
        if (_messageIsAltSpaceKeypress(message))
        {
            _host->OnDirectKeyEvent(VK_SPACE, LOBYTE(HIWORD(message.lParam)), true);
            continue;
        }

        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    return 0;
}

uint64_t WindowThread::PeasantID()
{
    return _peasant.GetID();
}
