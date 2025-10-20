#include "ImGuiFileBrowser.h"
#include <cctype>

#include <algorithm>

namespace ImGui {

namespace {
bool MatchesFilter(const std::filesystem::directory_entry& entry,
                   const std::vector<const char*>& filters) {
    if (filters.empty()) {
        return true;
    }
    if (entry.is_directory()) {
        return true;
    }
    std::string extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (const char* filter : filters) {
        std::string f(filter);
        std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (!f.empty() && f[0] == '*') {
            f.erase(f.begin());  // remove leading '*'
        }
        if (!f.empty() && f[0] == '.') {
            if (extension == f) {
                return true;
            }
        }
    }
    return false;
}
}  // namespace

FileBrowser::FileBrowser(DialogMode mode) : mode_(mode) {
    pwd_ = std::filesystem::current_path();
    RefreshEntries();
}

void FileBrowser::SetTitle(const std::string& title) {
    title_ = title;
}

void FileBrowser::SetTypeFilters(const std::vector<const char*>& filters) {
    filters_ = filters;
}

void FileBrowser::SetPwd(const std::filesystem::path& path) {
    if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
        pwd_ = path;
        RefreshEntries();
    }
}

void FileBrowser::Open() {
    open_ = true;
    RefreshEntries();
}

void FileBrowser::Close() {
    open_ = false;
}

bool FileBrowser::IsOpened() const {
    return open_;
}

void FileBrowser::Display() {
    if (!open_) {
        return;
    }

    if (!ImGui::Begin(title_.c_str(), &open_, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (mode_ != DialogMode::SelectFolder) {
        ImGui::Text("Current directory: %s", pwd_.string().c_str());
    }

    ImGui::Separator();

    if (mode_ != DialogMode::SelectFolder) {
        ImGui::BeginChild("##filebrowser_scroller", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3));
    } else {
        ImGui::BeginChild("##filebrowser_scroller");
    }

    if (pwd_.has_parent_path()) {
        if (ImGui::Selectable("..")) {
            SetPwd(pwd_.parent_path());
        }
    }

    for (const auto& entry : entries_) {
        const bool isDir = entry.is_directory();
        const std::string name = entry.path().filename().string();
        if (isDir) {
            if (ImGui::Selectable((name + "/").c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    SetPwd(entry.path());
                }
            }
        } else if (MatchesFilter(entry, filters_)) {
            bool selected = selectedPath_ == entry.path();
            if (ImGui::Selectable(name.c_str(), selected)) {
            if (ImGui::IsMouseDoubleClicked(0)) {
                if (AcceptSelection()) {
                    open_ = false;
                }
            }
                selectedPath_ = entry.path();
            }
        }
    }

    ImGui::EndChild();

    if (mode_ == DialogMode::Open) {
        if (ImGui::Button("Open")) {
            if (AcceptSelection()) {
                open_ = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ClearSelected();
            open_ = false;
        }
    } else if (mode_ == DialogMode::SelectFolder) {
        if (ImGui::Button("Select Folder")) {
            selectedPath_ = pwd_;
            open_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ClearSelected();
            open_ = false;
        }
    } else {  // Save
        if (ImGui::Button("Save")) {
            if (AcceptSelection()) {
                open_ = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ClearSelected();
            open_ = false;
        }
    }

    ImGui::End();
}

bool FileBrowser::HasSelected() const {
    return !selectedPath_.empty();
}

std::filesystem::path FileBrowser::GetSelected() const {
    return selectedPath_;
}

void FileBrowser::ClearSelected() {
    selectedPath_.clear();
}

void FileBrowser::RefreshEntries() {
    entries_.clear();
    if (!std::filesystem::exists(pwd_) || !std::filesystem::is_directory(pwd_)) {
        return;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(pwd_)) {
            entries_.push_back(entry);
        }
        std::sort(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
            if (a.is_directory() != b.is_directory()) {
                return a.is_directory() && !b.is_directory();
            }
            return a.path().filename() < b.path().filename();
        });
    } catch (const std::exception&) {
        // ignore errors while enumerating directories
    }
}

bool FileBrowser::AcceptSelection() {
    if (selectedPath_.empty()) {
        return false;
    }
    if (mode_ == DialogMode::Save) {
        if (selectedPath_.has_extension()) {
            return true;
        }
        // If user typed a directory name for save, treat as folder
        return false;
    }
    if (mode_ == DialogMode::SelectFolder) {
        if (std::filesystem::is_directory(selectedPath_)) {
            return true;
        }
        return false;
    }
    return std::filesystem::exists(selectedPath_);
}

}  // namespace ImGui
