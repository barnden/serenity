/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "Tab.h"
#include "BookmarksBarWidget.h"
#include "ConsoleWidget.h"
#include "DownloadWidget.h"
#include "InspectorWidget.h"
#include "WindowActions.h"
#include <AK/StringBuilder.h>
#include <LibGUI/Action.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/Clipboard.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MenuBar.h>
#include <LibGUI/StatusBar.h>
#include <LibGUI/TabWidget.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/ToolBar.h>
#include <LibGUI/ToolBarContainer.h>
#include <LibGUI/Window.h>
#include <LibJS/Interpreter.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOMTreeModel.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Frame/Frame.h>
#include <LibWeb/Layout/LayoutBlock.h>
#include <LibWeb/Layout/LayoutDocument.h>
#include <LibWeb/Layout/LayoutInline.h>
#include <LibWeb/Layout/LayoutNode.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/PageView.h>
#include <LibWeb/Parser/CSSParser.h>

namespace Browser {

extern String g_home_url;

URL url_from_user_input(const String& input)
{
    auto url = URL(input);
    if (url.is_valid())
        return url;

    StringBuilder builder;
    builder.append("http://");
    builder.append(input);
    return URL(builder.build());
}

Tab::Tab()
{
    auto& widget = *this;
    set_layout<GUI::VerticalBoxLayout>();

    m_toolbar_container = widget.add<GUI::ToolBarContainer>();
    auto& toolbar = m_toolbar_container->add<GUI::ToolBar>();
    m_page_view = widget.add<Web::PageView>();

    m_go_back_action = GUI::CommonActions::make_go_back_action([this](auto&) {
        m_history.go_back();
        update_actions();
        TemporaryChange<bool> change(m_should_push_loads_to_history, false);
        m_page_view->load(m_history.current());
    },
        this);

    m_go_forward_action = GUI::CommonActions::make_go_forward_action([this](auto&) {
        m_history.go_forward();
        update_actions();
        TemporaryChange<bool> change(m_should_push_loads_to_history, false);
        m_page_view->load(m_history.current());
    },
        this);

    toolbar.add_action(*m_go_back_action);
    toolbar.add_action(*m_go_forward_action);

    toolbar.add_action(GUI::CommonActions::make_go_home_action([this](auto&) {
        m_page_view->load(g_home_url);
    },
        this));

    m_reload_action = GUI::CommonActions::make_reload_action([this](auto&) {
        TemporaryChange<bool> change(m_should_push_loads_to_history, false);
        m_page_view->reload();
    },
        this);

    toolbar.add_action(*m_reload_action);

    m_location_box = toolbar.add<GUI::TextBox>();
    m_location_box->set_size_policy(GUI::SizePolicy::Fill, GUI::SizePolicy::Fixed);
    m_location_box->set_preferred_size(0, 22);

    m_location_box->on_return_pressed = [this] {
        auto url = url_from_user_input(m_location_box->text());
        m_page_view->load(url);
        m_page_view->set_focus(true);
    };

    m_location_box->add_custom_context_menu_action(GUI::Action::create("Paste & Go", [this](auto&) {
        m_location_box->set_text(GUI::Clipboard::the().data());
        m_location_box->on_return_pressed();
    }));

    m_bookmark_button = toolbar.add<GUI::Button>();
    m_bookmark_button->set_button_style(Gfx::ButtonStyle::CoolBar);
    m_bookmark_button->set_icon(Gfx::Bitmap::load_from_file("/res/icons/16x16/bookmark-contour.png"));
    m_bookmark_button->set_size_policy(GUI::SizePolicy::Fixed, GUI::SizePolicy::Fixed);
    m_bookmark_button->set_preferred_size(22, 22);

    m_bookmark_button->on_click = [this](auto) {
        auto url = m_page_view->document()->url().to_string();
        if (BookmarksBarWidget::the().contains_bookmark(url)) {
            BookmarksBarWidget::the().remove_bookmark(url);
        } else {
            BookmarksBarWidget::the().add_bookmark(url, m_title);
        }
        update_bookmark_button(url);
    };

    m_page_view->on_load_start = [this](auto& url) {
        m_location_box->set_icon(nullptr);
        m_location_box->set_text(url.to_string());
        if (m_should_push_loads_to_history)
            m_history.push(url);
        update_actions();
        update_bookmark_button(url.to_string());
    };

    m_page_view->on_link_click = [this](auto& href, auto& target, unsigned modifiers) {
        if (target == "_blank" || modifiers == Mod_Ctrl) {
            auto url = m_page_view->document()->complete_url(href);
            on_tab_open_request(url);
        } else {
            if (href.starts_with("#")) {
                auto anchor = href.substring_view(1, href.length() - 1);
                m_page_view->scroll_to_anchor(anchor);
            } else {
                auto url = m_page_view->document()->complete_url(href);
                m_page_view->load(url);
            }
        }
    };

    m_link_context_menu = GUI::Menu::construct();
    m_link_context_menu->add_action(GUI::Action::create("Open", [this](auto&) {
        m_page_view->on_link_click(m_link_context_menu_href, "", 0);
    }));
    m_link_context_menu->add_action(GUI::Action::create("Open in new tab", [this](auto&) {
        m_page_view->on_link_click(m_link_context_menu_href, "_blank", 0);
    }));
    m_link_context_menu->add_action(GUI::Action::create("Copy link", [this](auto&) {
        GUI::Clipboard::the().set_data(m_page_view->document()->complete_url(m_link_context_menu_href).to_string());
    }));
    m_link_context_menu->add_separator();
    m_link_context_menu->add_action(GUI::Action::create("Download", [this](auto&) {
        auto window = GUI::Window::construct();
        window->set_rect(300, 300, 300, 150);
        auto url = m_page_view->document()->complete_url(m_link_context_menu_href);
        window->set_title(String::format("0%% of %s", url.basename().characters()));
        window->set_resizable(false);
        window->set_main_widget<DownloadWidget>(url);
        window->show();
        (void)window.leak_ref();
    }));

    m_page_view->on_link_context_menu_request = [this](auto& href, auto& screen_position) {
        m_link_context_menu_href = href;
        m_link_context_menu->popup(screen_position);
    };

    m_page_view->on_link_middle_click = [this](auto& href) {
        m_page_view->on_link_click(href, "_blank", 0);
    };

    m_page_view->on_title_change = [this](auto& title) {
        if (title.is_null()) {
            m_title = m_page_view->document()->url().to_string();
        } else {
            m_title = title;
        }
        if (on_title_change)
            on_title_change(m_title);
    };

    m_page_view->on_favicon_change = [this](auto& icon) {
        m_icon = icon;
        m_location_box->set_icon(&icon);
        if (on_favicon_change)
            on_favicon_change(icon);
    };

    m_page_view->on_set_document = [this](auto* document) {
        if (document && m_console_window) {
            auto* console_widget = static_cast<ConsoleWidget*>(m_console_window->main_widget());
            console_widget->set_interpreter(document->interpreter().make_weak_ptr());
        }
    };

    auto focus_location_box_action = GUI::Action::create(
        "Focus location box", { Mod_Ctrl, Key_L }, [this](auto&) {
            m_location_box->select_all();
            m_location_box->set_focus(true);
        },
        this);

    m_statusbar = widget.add<GUI::StatusBar>();

    m_page_view->on_link_hover = [this](auto& href) {
        m_statusbar->set_text(href);
    };

    m_page_view->on_url_drop = [this](auto& url) {
        m_page_view->load(url);
    };

    m_menubar = GUI::MenuBar::construct();

    auto& app_menu = m_menubar->add_menu("Browser");
    app_menu.add_action(WindowActions::the().create_new_tab_action());
    app_menu.add_action(GUI::Action::create(
        "Close tab", { Mod_Ctrl, Key_W }, Gfx::Bitmap::load_from_file("/res/icons/16x16/close-tab.png"), [this](auto&) {
            on_tab_close_request(*this);
        },
        this));

    app_menu.add_action(*m_reload_action);
    app_menu.add_separator();
    app_menu.add_action(GUI::CommonActions::make_quit_action([](auto&) {
        GUI::Application::the().quit();
    }));

    auto& view_menu = m_menubar->add_menu("View");
    view_menu.add_action(GUI::CommonActions::make_fullscreen_action(
        [this](auto&) {
            window()->set_fullscreen(!window()->is_fullscreen());

            auto is_fullscreen = window()->is_fullscreen();
            auto* tab_widget = static_cast<GUI::TabWidget*>(parent_widget());
            tab_widget->set_bar_visible(!is_fullscreen && tab_widget->children().size() > 1);
            m_toolbar_container->set_visible(!is_fullscreen);
            m_statusbar->set_visible(!is_fullscreen);
        },
        this));

    auto view_source_action = GUI::Action::create(
        "View source", { Mod_Ctrl, Key_U }, [this](auto&) {
            ASSERT(m_page_view->document());
            auto url = m_page_view->document()->url().to_string();
            auto source = m_page_view->document()->source();
            auto window = GUI::Window::construct();
            auto& editor = window->set_main_widget<GUI::TextEditor>();
            editor.set_text(source);
            editor.set_readonly(true);
            editor.set_ruler_visible(true);
            window->set_rect(150, 150, 640, 480);
            window->set_title(url);
            window->show();
            (void)window.leak_ref();
        },
        this);

    auto inspect_dom_tree_action = GUI::Action::create(
        "Inspect DOM tree", { Mod_None, Key_F12 }, [this](auto&) {
            if (!m_dom_inspector_window) {
                m_dom_inspector_window = GUI::Window::construct();
                m_dom_inspector_window->set_rect(100, 100, 300, 500);
                m_dom_inspector_window->set_title("DOM inspector");
                m_dom_inspector_window->set_main_widget<InspectorWidget>();
            }
            auto* inspector_widget = static_cast<InspectorWidget*>(m_dom_inspector_window->main_widget());
            inspector_widget->set_document(m_page_view->document());
            m_dom_inspector_window->show();
            m_dom_inspector_window->move_to_front();
        },
        this);

    auto& inspect_menu = m_menubar->add_menu("Inspect");
    inspect_menu.add_action(*view_source_action);
    inspect_menu.add_action(*inspect_dom_tree_action);

    inspect_menu.add_action(GUI::Action::create(
        "Open JS Console", { Mod_Ctrl, Key_I }, [this](auto&) {
            if (!m_console_window) {
                m_console_window = GUI::Window::construct();
                m_console_window->set_rect(100, 100, 500, 300);
                m_console_window->set_title("JS Console");
                m_console_window->set_main_widget<ConsoleWidget>();
            }
            auto* console_widget = static_cast<ConsoleWidget*>(m_console_window->main_widget());
            console_widget->set_interpreter(m_page_view->document()->interpreter().make_weak_ptr());
            m_console_window->show();
            m_console_window->move_to_front();
        },
        this));

    auto& debug_menu = m_menubar->add_menu("Debug");
    debug_menu.add_action(GUI::Action::create(
        "Dump DOM tree", [this](auto&) {
            Web::dump_tree(*m_page_view->document());
        },
        this));
    debug_menu.add_action(GUI::Action::create(
        "Dump Layout tree", [this](auto&) {
            Web::dump_tree(*m_page_view->document()->layout_node());
        },
        this));
    debug_menu.add_action(GUI::Action::create(
        "Dump Style sheets", [this](auto&) {
            for (auto& sheet : m_page_view->document()->style_sheets().sheets()) {
                dump_sheet(sheet);
            }
        },
        this));
    debug_menu.add_separator();
    auto line_box_borders_action = GUI::Action::create_checkable(
        "Line box borders", [this](auto& action) {
            m_page_view->set_should_show_line_box_borders(action.is_checked());
            m_page_view->update();
        },
        this);
    line_box_borders_action->set_checked(false);
    debug_menu.add_action(line_box_borders_action);

    auto& bookmarks_menu = m_menubar->add_menu("Bookmarks");
    bookmarks_menu.add_action(WindowActions::the().show_bookmarks_bar_action());

    auto& help_menu = m_menubar->add_menu("Help");
    help_menu.add_action(WindowActions::the().about_action());

    m_tab_context_menu = GUI::Menu::construct();
    m_tab_context_menu->add_action(GUI::Action::create("Reload Tab", [this](auto&) {
        m_reload_action->activate();
    }));
    m_tab_context_menu->add_action(GUI::Action::create("Close Tab", [this](auto&) {
        on_tab_close_request(*this);
    }));

    m_page_context_menu = GUI::Menu::construct();
    m_page_context_menu->add_action(*m_go_back_action);
    m_page_context_menu->add_action(*m_go_forward_action);
    m_page_context_menu->add_action(*m_reload_action);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(*view_source_action);
    m_page_context_menu->add_action(*inspect_dom_tree_action);
    m_page_view->on_context_menu_request = [&](auto& screen_position) {
        m_page_context_menu->popup(screen_position);
    };
}

Tab::~Tab()
{
}

void Tab::load(const URL& url)
{
    m_page_view->load(url);
}

void Tab::update_actions()
{
    m_go_back_action->set_enabled(m_history.can_go_back());
    m_go_forward_action->set_enabled(m_history.can_go_forward());
}

void Tab::update_bookmark_button(const String& url)
{
    if (BookmarksBarWidget::the().contains_bookmark(url)) {
        m_bookmark_button->set_icon(Gfx::Bitmap::load_from_file("/res/icons/16x16/bookmark-filled.png"));
        m_bookmark_button->set_tooltip("Remove Bookmark");
    } else {
        m_bookmark_button->set_icon(Gfx::Bitmap::load_from_file("/res/icons/16x16/bookmark-contour.png"));
        m_bookmark_button->set_tooltip("Add Bookmark");
    }
}

void Tab::did_become_active()
{
    Web::ResourceLoader::the().on_load_counter_change = [this] {
        if (Web::ResourceLoader::the().pending_loads() == 0) {
            m_statusbar->set_text("");
            return;
        }
        m_statusbar->set_text(String::format("Loading (%d pending resources...)", Web::ResourceLoader::the().pending_loads()));
    };

    BookmarksBarWidget::the().on_bookmark_click = [this](auto& url, unsigned modifiers) {
        if (modifiers & Mod_Ctrl)
            on_tab_open_request(url);
        else
            m_page_view->load(url);
    };

    BookmarksBarWidget::the().on_bookmark_hover = [this](auto&, auto& url) {
        m_statusbar->set_text(url);
    };

    BookmarksBarWidget::the().remove_from_parent();
    m_toolbar_container->add_child(BookmarksBarWidget::the());

    auto is_fullscreen = window()->is_fullscreen();
    m_toolbar_container->set_visible(!is_fullscreen);
    m_statusbar->set_visible(!is_fullscreen);

    GUI::Application::the().set_menubar(m_menubar);
}

void Tab::context_menu_requested(const Gfx::IntPoint& screen_position)
{
    m_tab_context_menu->popup(screen_position);
}

}
