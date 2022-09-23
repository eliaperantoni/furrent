#include <iostream>

#include "gui/gui.cpp"
#include "log/logger.hpp"
#include "raygui.h"
#include "raylib.h"
#define GUI_FILE_DIALOG_IMPLEMENTATION
#include "gui/file_dialog.h"
#define GUI_FILE_DIALOG_IMPLEMENTATION

using namespace fur;

// Function to add a new torrent, it is called when the user clicks on the add
void add_torrent(GuiFileDialogState *file_dialog_state,
                 fur::gui::GuiScrollTorrentState *scroll_state) {
  // Add the torrent to the current list of torrents
  fur::gui::TorrentGui torrent{file_dialog_state->realFileName, fur::gui::STOP,
                               0};
  scroll_state->torrents.push_back(torrent);
  // Close the dialog
  file_dialog_state->SelectFilePressed = false;
  file_dialog_state->fileDialogActive = false;
}

void remove_torrent(fur::gui::GuiScrollTorrentState *scroll_state,
                    fur::gui::GuiConfirmDialogState *confirm_dialog_state) {
  // Open the confirm dialog if it is not already open or have been clicked
  if (!confirm_dialog_state->show && !confirm_dialog_state->clicked) {
    confirm_dialog_state->show = true;
    confirm_dialog_state->clicked = false;
    confirm_dialog_state->message =
        "Are you sure you want to delete the torrent?";
    confirm_dialog_state->button_yes = "Yes";
    confirm_dialog_state->button_no = "No";
  }
  // If the user clicked on an option of the confirm dialog
  if (confirm_dialog_state->clicked) {
    // Confirm the deletion
    if (confirm_dialog_state->confirm) {
      scroll_state->torrents.erase(
          scroll_state->torrents.begin() +
          scroll_state->torrent_dialog_state.torrent.progress);
      // TODO: delete real torrent
    }
    // Close the dialog
    confirm_dialog_state->show = false;
    confirm_dialog_state->confirm = false;
    confirm_dialog_state->clicked = false;
    // Close the torrent dialog
    scroll_state->torrent_dialog_state.delete_torrent = false;
  }
}

void update_settings(fur::gui::GuiSettingsDialogState *settings_dialog_state) {
  if(settings_dialog_state->updated_path){
    // TODO: do something with the new path
    settings_dialog_state->updated_path = false;
    settings_dialog_state->show = false;
  }
}
int main() {
  fur::log::initialize_custom_logger();
  auto logger = spdlog::get("custom");
  // Set the window configuration
  fur::gui::setup_config();
  // Create all the states
  GuiFileDialogState file_state =
      InitGuiFileDialog(550, 500, GetWorkingDirectory(), false, ".torrent");
  fur::gui::GuiSettingsDialogState settings_state{};
  fur::gui::GuiScrollTorrentState scroll_state{
      Vector2{},
      {{"A", fur::gui::TorrentState::COMPLETED, 100},
       {"B", fur::gui::TorrentState::DOWNLOAD, 75},
       {"C", fur::gui::TorrentState::STOP, 50},
       {"D", fur::gui::TorrentState::ERROR, 25}},
      fur::gui::GuiTorrentDialogState{}};
  fur::gui::GuiConfirmDialogState confirm_dialog_state{};
  // Main loop
  while (!WindowShouldClose()) {
    BeginDrawing();
    ClearBackground(RAYWHITE);
    // ------
    // Drawing the page
    // ------
    // Title
    GuiDrawText("Furrent", {gui::BORDER, gui::BORDER, 0, 50}, TEXT_ALIGN_LEFT,
                BLACK);
    auto button_file_dialog = GuiButton(
        {gui::W_WIDTH - gui::BORDER - 190, 10, 150, 30}, "#3# Open torrent");

    auto button_settings =
        GuiButton({gui::W_WIDTH - gui::BORDER - 30, 10, 30, 30}, "#141#");
    // Scroll panel
    GuiScrollPanel({gui::BORDER, 100, gui::W_WIDTH - gui::BORDER * 2,
                    gui::W_HEIGHT - 100 - gui::BORDER},
                   NULL,
                   {gui::BORDER, 50, gui::W_WIDTH - 150,
                    static_cast<float>(50 * scroll_state.torrents.size())},
                   &scroll_state.scroll);
    // Drawing torrents
    fur::gui::draw_torrents(&scroll_state);
    // Scroll panel head
    GuiDrawRectangle({gui::BORDER, 61, gui::W_WIDTH - gui::BORDER * 2, 40}, 1,
                     gui::BORDER_COLOR, gui::PRIMARY_COLOR);
    GuiDrawText("Torrents",
                {gui::BORDER, 61, gui::W_WIDTH - gui::BORDER * 2, 40},
                TEXT_ALIGN_CENTER, gui::TEXT_COLOR);
    // Scroll panel bottom
    GuiDrawRectangle(Rectangle{gui::BORDER, gui::W_HEIGHT - gui::BORDER,
                               gui::W_WIDTH, gui::W_WIDTH},
                     1, gui::BACKGROUND_COLOR, gui::BACKGROUND_COLOR);

    // ------
    // Events
    // ------
    // Event caught for updating the scroll panel
    float wheelMove = GetMouseWheelMove();
    if (wheelMove != 0) {
      scroll_state.scroll.y += wheelMove * 20;
    }
    // Button add torrent
    if (button_file_dialog) {
      file_state.fileDialogActive = true;
    }
    // Button furrent settings
    if (button_settings) {
      settings_state.show = true;
    }
    // Action on filedialog
    if (file_state.SelectFilePressed) {
      if (IsFileExtension(file_state.fileNameText, ".torrent")) {
        add_torrent(&file_state, &scroll_state);
      } else {
        file_state.SelectFilePressed = false;
        file_state.fileDialogActive = true;
      }
    }
    // Action on torrent dialog
    if (scroll_state.torrent_dialog_state.delete_torrent) {
      remove_torrent(&scroll_state, &confirm_dialog_state);
    }
    // Action on settings dialog
    if (settings_state.show) {
      update_settings(&settings_state);
    }

    // Update dialogs
    GuiFileDialog(&file_state);
    settings_dialog(&settings_state);
    torrent_dialog(&scroll_state.torrent_dialog_state);
    confirm_dialog(&confirm_dialog_state);
    EndDrawing();
  }

  CloseWindow();
}
