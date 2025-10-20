#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <filesystem>

namespace ImGui {

class FileBrowser {
public:
    enum class DialogMode {
        Open,
        SelectFolder,
        Save
    };

    explicit FileBrowser(DialogMode mode = DialogMode::Open);
    void SetTitle(const std::string& title);
    void SetTypeFilters(const std::vector<const char*>& filters);
    void SetPwd(const std::filesystem::path& path);
    void Open();
    void Close();
    bool IsOpened() const;
    void Display();
    bool HasSelected() const;
    std::filesystem::path GetSelected() const;
    void ClearSelected();

private:
    void RefreshEntries();
    bool AcceptSelection();

    DialogMode mode_;
    std::string title_ = "File Browser";
    std::vector<const char*> filters_;
    std::filesystem::path pwd_;
    std::vector<std::filesystem::directory_entry> entries_;
    std::filesystem::path selectedPath_;
    bool open_ = false;
};

}  // namespace ImGui
