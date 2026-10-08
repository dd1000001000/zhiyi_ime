// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_SERVER_UI_PRESENTATION_CONTROLLER_H_
#define CXXIME_SERVER_UI_PRESENTATION_CONTROLLER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cxxime/config.h>
#include <cxxime/ui_channel.h>

class UiPresentationController {
public:
    using CommandHandler = std::function<void(cxxime::UiEndpointId, const cxxime::UiCommand&)>;

    UiPresentationController();
    ~UiPresentationController();

    UiPresentationController(const UiPresentationController&) = delete;
    UiPresentationController& operator=(const UiPresentationController&) = delete;

    bool start(const std::shared_ptr<const cxxime::Config>& config, CommandHandler command_handler);
    void stop();

    void present(cxxime::UiEndpointId endpoint, const cxxime::UiPresentationSnapshot* snapshot,
                 std::uint64_t candidate_placement_cycle,
                 std::uint64_t router_revision);
    void update_config(const std::shared_ptr<const cxxime::Config>& config);
    // Offline translations that arrived after the page was shown (machine_translator.h):
    // candidates of the latest page with no translation and one of these texts get it.
    void add_glosses(const std::vector<std::pair<std::string, std::string>>& glosses);
    std::uint32_t visible_candidate_count(
        std::uint32_t session_id, const cxxime::CandidateUiContext& context) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // CXXIME_SERVER_UI_PRESENTATION_CONTROLLER_H_
