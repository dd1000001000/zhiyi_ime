// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_SERVER_UI_PRESENTATION_ROUTER_H_
#define CXXIME_SERVER_UI_PRESENTATION_ROUTER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <cxxime/ui_channel.h>

class UiPresentationRouter {
public:
    using PresentationHandler =
        std::function<void(cxxime::UiEndpointId, const cxxime::UiPresentationSnapshot*,
                           std::uint64_t candidate_placement_cycle,
                           std::uint64_t router_revision)>;

    UiPresentationRouter();
    ~UiPresentationRouter();

    UiPresentationRouter(const UiPresentationRouter&) = delete;
    UiPresentationRouter& operator=(const UiPresentationRouter&) = delete;

    bool start(PresentationHandler presentation_handler,
               const std::wstring& pipe_name = cxxime::UI_PIPE_BASE_NAME);
    void stop();

    bool send_command(cxxime::UiEndpointId endpoint, const cxxime::UiCommand& command);
    void reconcile_system_ui(bool clear_presentation);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // CXXIME_SERVER_UI_PRESENTATION_ROUTER_H_
