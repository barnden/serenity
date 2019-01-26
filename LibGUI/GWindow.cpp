#include "GWindow.h"
#include "GEvent.h"
#include "GEventLoop.h"
#include "GWidget.h"
#include <SharedGraphics/GraphicsBitmap.h>
#include <LibC/gui.h>
#include <LibC/stdio.h>
#include <LibC/stdlib.h>
#include <LibC/unistd.h>
#include <AK/HashMap.h>

static HashMap<int, GWindow*>* s_windows;

static HashMap<int, GWindow*>& windows()
{
    if (!s_windows)
        s_windows = new HashMap<int, GWindow*>;
    return *s_windows;
}

GWindow* GWindow::from_window_id(int window_id)
{
    auto it = windows().find(window_id);
    if (it != windows().end())
        return (*it).value;
    return nullptr;
}

GWindow::GWindow(GObject* parent)
    : GObject(parent)
{
    GUI_WindowParameters wparams;
    wparams.rect = { { 100, 400 }, { 140, 140 } };
    wparams.background_color = 0xffc0c0;
    strcpy(wparams.title, "GWindow");
    m_window_id = gui_create_window(&wparams);
    if (m_window_id < 0) {
        perror("gui_create_window");
        exit(1);
    }

    windows().set(m_window_id, this);
}

GWindow::~GWindow()
{
}

void GWindow::set_title(String&& title)
{
    dbgprintf("GWindow::set_title \"%s\"\n", title.characters());
    int rc = gui_set_window_title(m_window_id, title.characters(), title.length());
    ASSERT(rc == 0);
}

String GWindow::title() const
{
    char buffer[256];
    int rc = gui_get_window_title(m_window_id, buffer, sizeof(buffer));
    ASSERT(rc >= 0);
    return String(buffer, rc);
}

void GWindow::set_rect(const Rect& a_rect)
{
    dbgprintf("GWindow::set_rect! %d,%d %dx%d\n", a_rect.x(), a_rect.y(), a_rect.width(), a_rect.height());
    GUI_Rect rect = a_rect;
    int rc = gui_set_window_rect(m_window_id, &rect);
    ASSERT(rc == 0);
}

void GWindow::event(GEvent& event)
{
    if (event.is_mouse_event()) {
        if (!m_main_widget)
            return;
        auto& mouse_event = static_cast<GMouseEvent&>(event);
        if (m_main_widget) {
            auto result = m_main_widget->hit_test(mouse_event.x(), mouse_event.y());
            auto local_event = make<GMouseEvent>(event.type(), Point { result.localX, result.localY }, mouse_event.buttons(), mouse_event.button());
            ASSERT(result.widget);
            return result.widget->event(*local_event);
        }
    }

    if (event.is_paint_event()) {
        if (!m_main_widget)
            return;
        auto& paint_event = static_cast<GPaintEvent&>(event);
        auto rect = paint_event.rect();
        if (rect.is_empty())
            rect = m_main_widget->rect();
        m_main_widget->event(*make<GPaintEvent>(rect));
        GUI_Rect gui_rect = rect;
        int rc = gui_notify_paint_finished(m_window_id, &gui_rect);
        ASSERT(rc == 0);
    }

    if (event.is_key_event()) {
        if (!m_focused_widget)
            return;
        return m_focused_widget->event(event);
    }

    return GObject::event(event);
}

bool GWindow::is_visible() const
{
    return false;
}

void GWindow::close()
{
}

void GWindow::show()
{
}

void GWindow::update(const Rect& a_rect)
{
    GUI_Rect rect = a_rect;
    int rc = gui_invalidate_window(m_window_id, a_rect.is_null() ? nullptr : &rect);
    ASSERT(rc == 0);
}

void GWindow::set_main_widget(GWidget* widget)
{
    if (m_main_widget == widget)
        return;
    m_main_widget = widget;
    if (widget)
        widget->set_window(this);
    update();
}

void GWindow::set_focused_widget(GWidget* widget)
{
    if (m_focused_widget == widget)
        return;
    if (m_focused_widget) {
        GEventLoop::main().post_event(m_focused_widget, make<GEvent>(GEvent::FocusOut));
        m_focused_widget->update();
    }
    m_focused_widget = widget;
    if (m_focused_widget) {
        GEventLoop::main().post_event(m_focused_widget, make<GEvent>(GEvent::FocusIn));
        m_focused_widget->update();
    }
}
