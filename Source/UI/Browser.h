#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "EditorContext.h"
#include "Widgets.h"

namespace labx0::ui
{
    // LIBRARY // СХРОН: the USER library, the ZONE (STALKER SPECIMENS), FL's stalker PACK and the curated ZONE KIT.
    // Searches run on a background thread a moment after the last keystroke, so typing never waits for 200,000
    // file names. Click previews (when PREVIEW is on), double click or Enter loads the sound into the selected
    // lane, rows can be dragged onto any lane.
    class BrowserPanel final : public juce::Component,
                               public juce::ListBoxModel,
                               private juce::Timer
    {
    public:
        explicit BrowserPanel (EditorContext& context);
        ~BrowserPanel() override;

        void resized() override;
        void paint (juce::Graphics&) override;

        int getNumRows() override;
        void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
        void listBoxItemClicked (int row, const juce::MouseEvent&) override;
        void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
        void returnKeyPressed (int row) override;
        juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override;
        juce::String getTooltipForRow (int row) override;

        std::function<void (int root)> onChooseFolder;

    private:
        enum Tab { tabUser = 0, tabZone, tabPack, tabKit, numTabs };

        void timerCallback() override;
        void setTab (int t);
        void setCategory (int c);
        void startQuery();
        void rebuildKitRows();
        SampleRef refForRow (int row) const;
        juce::String nameForRow (int row) const;
        void assignRow (int row);
        void previewRow (int row);
        void updateStatus();

        EditorContext& ctx;
        Library& library;

        std::array<juce::TextButton, numTabs> tabButtons;
        std::array<juce::TextButton, 10> categoryButtons;
        juce::TextEditor search;
        juce::ComboBox sortBox;
        juce::ListBox list { "library", this };
        juce::TextButton previewButton { "PREVIEW" }, playButton { "PLAY" }, stopButton { "STOP" }, assignButton { "LOAD" }, favButton { "FAV" };
        juce::TextButton folderButton { "FOLDER..." }, rescanButton { "RESCAN" };
        juce::Label status;

        int tab = tabUser, category = 0;
        std::shared_ptr<const RootIndex> index;
        std::vector<int> rows;
        int seenIndexVersion = -1, seenRootsVersion = -1;
        juce::String lastQueryKey;
        int queryDelay = -1;
        std::atomic<int> queryGeneration { 0 };
        juce::ThreadPool pool { juce::ThreadPoolOptions().withThreadName ("LAB X-0 search").withNumberOfThreads (1) };
        std::shared_ptr<bool> alive = std::make_shared<bool> (true);
        int resultRows = 0;
    };
}
