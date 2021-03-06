
#include "native_control_win.h"

#include <windowsx.h>

#include "base/logging.h"
#include "base/rtl.h"
#include "base/win_util.h"

#include "../focus/focus_manager.h"

namespace view
{

    // static
    const wchar_t* NativeControlWin::kNativeControlWinKey =
        L"__NATIVE_CONTROL_WIN__";

    ////////////////////////////////////////////////////////////////////////////////
    // NativeControlWin, public:

    NativeControlWin::NativeControlWin() {}

    NativeControlWin::~NativeControlWin()
    {
        HWND hwnd = native_view();
        if(hwnd)
        {
            // Destroy the hwnd if it still exists. Otherwise we won't have shut things
            // down correctly, leading to leaking and crashing if another message
            // comes in for the hwnd.
            Detach();
            DestroyWindow(hwnd);
        }
    }

    bool NativeControlWin::ProcessMessage(UINT message, WPARAM w_param,
        LPARAM l_param, LRESULT* result)
    {
        switch(message)
        {
        case WM_CONTEXTMENU:
            ShowContextMenu(gfx::Point(GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)));
            *result = 0;
            return true;
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORSTATIC:
            *result = GetControlColor(message, reinterpret_cast<HDC>(w_param),
                native_view());
            return true;
        }
        return false;
    }

    ////////////////////////////////////////////////////////////////////////////////
    // NativeControlWin, View overrides:

    void NativeControlWin::SetEnabled(bool enabled)
    {
        if(IsEnabled() != enabled)
        {
            View::SetEnabled(enabled);
            if(native_view())
            {
                EnableWindow(native_view(), IsEnabled());
            }
        }
    }

    void NativeControlWin::ViewHierarchyChanged(bool is_add, View* parent,
        View* child)
    {
        // Call the base class to hide the view if we're being removed.
        NativeViewHost::ViewHierarchyChanged(is_add, parent, child);

        // Create the HWND when we're added to a valid Widget. Many controls need a
        // parent HWND to function properly.
        if(is_add && GetWidget() && !native_view())
        {
            CreateNativeControl();
        }
    }

    void NativeControlWin::VisibilityChanged(View* starting_from, bool is_visible)
    {
        // We might get called due to visibility changes at any point in the
        // hierarchy, lets check whether we are really visible or not.
        bool visible = IsVisibleInRootView();
        if(!visible && native_view())
        {
            // We destroy the child control HWND when we become invisible because of the
            // performance cost of maintaining many HWNDs.
            HWND hwnd = native_view();
            Detach();
            DestroyWindow(hwnd);
        }
        else if(visible && !native_view())
        {
            if(GetWidget())
            {
                CreateNativeControl();
            }
        }
        if(visible)
        {
            // The view becomes visible after native control is created.
            // Layout now.
            Layout();
        }
    }

    void NativeControlWin::Focus()
    {
        DCHECK(native_view());
        SetFocus(native_view());

        // Since we are being wrapped by a view, accessibility should receive
        // the super class as the focused view.
        View* parent_view = GetParent();

        // Due to some controls not behaving as expected without having
        // a native win32 control, we don't always send a native (MSAA)
        // focus notification.
        // WLW TODO: fix it.
        bool send_native_event =
            //parent_view->GetClassName() != views::Combobox::kViewClassName &&
            parent_view->HasFocus();

        // Send the accessibility focus notification.
        parent_view->NotifyAccessibilityEvent(AccessibilityTypes::EVENT_FOCUS,
            send_native_event);
    }

    ////////////////////////////////////////////////////////////////////////////////
    // NativeControlWin, protected:

    void NativeControlWin::ShowContextMenu(const gfx::Point& location)
    {
        if(!GetContextMenuController())
        {
            return;
        }

        if(location.x()==-1 && location.y()==-1)
        {
            View::ShowContextMenu(GetKeyboardContextMenuLocation(), false);
        }
        else
        {
            View::ShowContextMenu(location, true);
        }
    }

    void NativeControlWin::NativeControlCreated(HWND native_control)
    {
        // Associate this object with the control's HWND so that WidgetWin can find
        // this object when it receives messages from it.
        // Note that we never unset this property. We don't have to.
        SetProp(native_control, kNativeControlWinKey, this);

        // Subclass so we get WM_KEYDOWN and WM_SETFOCUS messages.
        original_wndproc_ = base::SetWindowProc(native_control,
            &NativeControlWin::NativeControlWndProc);

        Attach(native_control);
        // native_view() is now valid.

        // Update the newly created HWND with any resident enabled state.
        EnableWindow(native_view(), IsEnabled());

        // This message ensures that the focus border is shown.
        SendMessage(native_view(), WM_CHANGEUISTATE,
            MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
    }

    DWORD NativeControlWin::GetAdditionalExStyle() const
    {
        // If the UI for the view is mirrored, we should make sure we add the
        // extended window style for a right-to-left layout so the subclass creates
        // a mirrored HWND for the underlying control.
        DWORD ex_style = 0;
        if(base::IsRTL())
        {
            ex_style |= base::GetExtendedStyles();
        }

        return ex_style;
    }

    DWORD NativeControlWin::GetAdditionalRTLStyle() const
    {
        // If the UI for the view is mirrored, we should make sure we add the
        // extended window style for a right-to-left layout so the subclass creates
        // a mirrored HWND for the underlying control.
        DWORD ex_style = 0;
        if(base::IsRTL())
        {
            ex_style |= base::GetExtendedTooltipStyles();
        }

        return ex_style;
    }

    ////////////////////////////////////////////////////////////////////////////////
    // NativeControlWin, private:

    LRESULT NativeControlWin::GetControlColor(UINT message, HDC dc, HWND sender)
    {
        View* ancestor = this;
        while(ancestor)
        {
            const Background* background = ancestor->background();
            if(background)
            {
                HBRUSH brush = background->GetNativeControlBrush();
                if(brush)
                {
                    return reinterpret_cast<LRESULT>(brush);
                }
            }
            ancestor = ancestor->GetParent();
        }

        // COLOR_BTNFACE is the default for dialog box backgrounds.
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }

    // static
    LRESULT NativeControlWin::NativeControlWndProc(HWND window,
        UINT message,
        WPARAM w_param,
        LPARAM l_param)
    {
        NativeControlWin* native_control =
            static_cast<NativeControlWin*>(GetProp(window, kNativeControlWinKey));
        DCHECK(native_control);

        if(message==WM_KEYDOWN &&
            native_control->OnKeyDown(static_cast<int>(w_param)))
        {
            return 0;
        }
        else if(message == WM_SETFOCUS)
        {
            // Let the focus manager know that the focus changed.
            FocusManager* focus_manager = native_control->GetFocusManager();
            if(focus_manager)
            {
                focus_manager->SetFocusedView(native_control->focus_view());
            }
            else
            {
                NOTREACHED();
            }
        }
        else if(message == WM_DESTROY)
        {
            base::SetWindowProc(window, native_control->original_wndproc_);
        }

        return CallWindowProc(native_control->original_wndproc_, window, message,
            w_param, l_param);
    }

} //namespace view