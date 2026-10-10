#include "Browser.h"
#include "../KitCatalog.h"

namespace labx0::ui
{
    namespace
    {
        // Category buttons: ALL, the eight categories, then favourites.
        const char* categoryLabels[] = { "ALL", "KICK", "SNARE", "HAT", "CYM", "PERC", "BREAK", "FX", "VOX", "FAV" };

        Root rootForTab (int tab)
        {
            switch (tab)
            {
                case 1:  return Root::zone;
                case 2:  return Root::pack;
                default: return Root::user;
            }
        }

        int categoryForGroup (KitGroup g)
        {
            switch (g)
            {
                case KitGroup::ordnance: return catKick;
                case KitGroup::fire:     return catSnare;
                case KitGroup::click:    return catHat;
                case KitGroup::perc:     return catPerc;
                case KitGroup::vox:      return catVox;
                default:                 return catFx;
            }
        }

        juce::String formatLength (float seconds)
        {
            if (seconds < 0.0f)
                return {};
            if (seconds < 10.0f)
                return juce::String (seconds, 2) + "s";
            if (seconds < 60.0f)
                return juce::String (seconds, 1) + "s";
            return juce::String ((int) seconds / 60) + ":" + juce::String ((int) seconds % 60).paddedLeft ('0', 2);
        }
    }

    BrowserPanel::BrowserPanel (EditorContext& context)
        : ctx (context), library (context.processor.getLibrary())
    {
        const char* tabNames[] = { "USER", "ZONE", "PACK", "KIT" };
        const char* tabTips[] = { "Your sample library (FL Studio's legacy Packs folder by default)",
                                  "The STALKER SPECIMENS library extracted from your own games",
                                  "FL Studio's legacy 'stalker sounds' pack",
                                  "ZONE KIT: drum hits picked from the STALKER sounds" };
        for (int t = 0; t < numTabs; ++t)
        {
            auto& b = tabButtons[(size_t) t];
            b.setButtonText (tabNames[t]);
            styleButton (b, tabTips[t]);
            b.onClick = [this, t] { setTab (t); };
            addAndMakeVisible (b);
        }
        for (int c = 0; c < 10; ++c)
        {
            auto& b = categoryButtons[(size_t) c];
            b.setButtonText (categoryLabels[c]);
            styleButton (b, c == 9 ? juce::String ("Your favourites") : "Show " + juce::String (categoryLabels[c]).toLowerCase());
            b.onClick = [this, c] { setCategory (c); };
            addAndMakeVisible (b);
        }

        search.setTextToShowWhenEmpty ("SEARCH  (amen 174, snare dirty...)", Palette::dim.withAlpha (0.6f));
        search.setFont (monoFont (12.0f));
        search.setColour (juce::TextEditor::backgroundColourId, Palette::background);
        search.onTextChange = [this] { queryDelay = 4; };
        search.onReturnKey = [this] { if (! rows.empty()) assignRow (juce::jmax (0, list.getSelectedRow())); };
        search.onEscapeKey = [this] { search.clear(); queryDelay = 1; };
        addAndMakeVisible (search);

        sortBox.addItemList ({ "FOLDER", "NAME", "LENGTH", "BPM" }, 1);
        sortBox.setSelectedId (1, juce::dontSendNotification);
        sortBox.setTooltip ("Sort order");
        sortBox.onChange = [this] { queryDelay = 1; };
        addAndMakeVisible (sortBox);

        list.setRowHeight (17);
        list.setColour (juce::ListBox::backgroundColourId, Palette::background);
        list.setOutlineThickness (1);
        list.setMultipleSelectionEnabled (false);
        addAndMakeVisible (list);

        styleButton (previewButton, "Preview a sound when it is clicked", true);
        previewButton.setToggleState (true, juce::dontSendNotification);
        styleButton (playButton, "Preview the selected sound");
        playButton.onClick = [this] { previewRow (list.getSelectedRow()); };
        styleButton (stopButton, "Stop the preview");
        stopButton.onClick = [this] { ctx.processor.stopPreview(); };
        styleButton (assignButton, "Load the selected sound into the selected lane (double click or Enter does the same)");
        assignButton.onClick = [this] { assignRow (list.getSelectedRow()); };
        styleButton (favButton, "Add the selected sound to your favourites, or remove it");
        favButton.onClick = [this]
        {
            const auto ref = refForRow (list.getSelectedRow());
            if (! ref.isEmpty())
            {
                library.setFavourite (ref, ! library.isFavourite (ref));
                list.repaint();
                if (category == 9)
                    queryDelay = 1;
            }
        };
        styleButton (folderButton, "Choose the folder of this library (saved for every project)");
        folderButton.onClick = [this] { if (onChooseFolder && tab != tabKit) onChooseFolder ((int) rootForTab (tab)); };
        styleButton (rescanButton, "Scan this library again from scratch");
        rescanButton.onClick = [this] { if (tab != tabKit) library.requestScan (rootForTab (tab), true); };
        for (auto* b : { &previewButton, &playButton, &stopButton, &assignButton, &favButton, &folderButton, &rescanButton })
            addAndMakeVisible (*b);

        status.setFont (monoFont (10.0f));
        status.setColour (juce::Label::textColourId, Palette::dim);
        status.setJustificationType (juce::Justification::topLeft);
        status.setMinimumHorizontalScale (0.6f);
        addAndMakeVisible (status);

        setTab (tabUser);
        startTimerHz (20);
    }

    BrowserPanel::~BrowserPanel()
    {
        *alive = false;
        stopTimer();
        pool.removeAllJobs (true, 2000);
    }

    void BrowserPanel::paint (juce::Graphics&) {}

    void BrowserPanel::resized()
    {
        auto b = getLocalBounds();
        auto tabsRow = b.removeFromTop (22);
        const int tw = tabsRow.getWidth() / numTabs;
        for (auto& t : tabButtons)
            t.setBounds (tabsRow.removeFromLeft (tw).reduced (1, 1));
        b.removeFromTop (3);
        for (int row = 0; row < 2; ++row)
        {
            auto r = b.removeFromTop (20);
            const int cw = r.getWidth() / 5;
            for (int k = 0; k < 5; ++k)
                categoryButtons[(size_t) (row * 5 + k)].setBounds (r.removeFromLeft (cw).reduced (1, 1));
        }
        b.removeFromTop (4);
        auto searchRow = b.removeFromTop (24);
        sortBox.setBounds (searchRow.removeFromRight (72).reduced (1, 1));
        search.setBounds (searchRow.reduced (1, 1));
        b.removeFromTop (4);

        auto bottom = b.removeFromBottom (22);
        folderButton.setBounds (bottom.removeFromLeft (bottom.getWidth() / 2).reduced (1, 1));
        rescanButton.setBounds (bottom.reduced (1, 1));
        status.setBounds (b.removeFromBottom (30));
        auto buttons = b.removeFromBottom (22);
        const int bw = buttons.getWidth() / 5;
        previewButton.setBounds (buttons.removeFromLeft (bw + 8).reduced (1, 1));
        playButton.setBounds (buttons.removeFromLeft (bw - 2).reduced (1, 1));
        stopButton.setBounds (buttons.removeFromLeft (bw - 2).reduced (1, 1));
        assignButton.setBounds (buttons.removeFromLeft (bw - 2).reduced (1, 1));
        favButton.setBounds (buttons.reduced (1, 1));
        b.removeFromBottom (3);
        list.setBounds (b);
    }

    void BrowserPanel::setTab (int t)
    {
        tab = juce::jlimit (0, numTabs - 1, t);
        for (int k = 0; k < numTabs; ++k)
            tabButtons[(size_t) k].setToggleState (k == tab, juce::dontSendNotification);
        folderButton.setEnabled (tab != tabKit);
        rescanButton.setEnabled (tab != tabKit);
        if (tab != tabKit)
            library.requestScan (rootForTab (tab));
        index.reset();
        seenIndexVersion = -1;
        lastQueryKey.clear();
        queryDelay = 1;
        setCategory (category);
    }

    void BrowserPanel::setCategory (int c)
    {
        category = juce::jlimit (0, 9, c);
        for (int k = 0; k < 10; ++k)
            categoryButtons[(size_t) k].setToggleState (k == category, juce::dontSendNotification);
        queryDelay = 1;
    }

    void BrowserPanel::timerCallback()
    {
        if (tab != tabKit)
        {
            const auto r = rootForTab (tab);
            const int v = library.getIndexVersion (r);
            if (v != seenIndexVersion)
            {
                seenIndexVersion = v;
                index = library.getIndex (r);
                lastQueryKey.clear();
                queryDelay = std::max (queryDelay, 1);
            }
            const int rv = library.getRootsVersion();
            if (rv != seenRootsVersion)
            {
                seenRootsVersion = rv;
                library.requestScan (r);
            }
        }

        if (queryDelay > 0 && --queryDelay == 0)
            startQuery();
        updateStatus();
    }

    void BrowserPanel::startQuery()
    {
        const auto text = search.getText().trim();
        const auto key = juce::String (tab) + "|" + juce::String (category) + "|" + juce::String (sortBox.getSelectedId()) + "|"
                       + text + "|" + juce::String (seenIndexVersion);
        if (key == lastQueryKey)
            return;
        lastQueryKey = key;

        if (tab == tabKit)
        {
            rebuildKitRows();
            return;
        }
        if (index == nullptr)
        {
            rows.clear();
            list.updateContent();
            list.repaint();
            return;
        }

        Query q;
        q.root = rootForTab (tab);
        q.category = category == 9 ? -1 : category;
        q.text = text;
        q.sort = sortBox.getSelectedId() - 1;
        auto snapshot = index;
        auto favourites = std::make_shared<std::vector<juce::String>> (library.getFavourites());
        const int generation = ++queryGeneration;
        auto token = alive;
        juce::Component::SafePointer<BrowserPanel> safe (this);

        pool.addJob ([snapshot, q, favourites, generation, token, safe]
        {
            auto result = std::make_shared<std::vector<int>> (runQuery (*snapshot, q, favourites.get()));
            juce::MessageManager::callAsync ([safe, result, generation, token, snapshot]
            {
                if (! *token || safe == nullptr || generation != safe->queryGeneration.load())
                    return;
                if (safe->index != snapshot)
                    return;
                safe->rows = std::move (*result);
                safe->list.updateContent();
                safe->list.deselectAllRows();
                safe->list.scrollToEnsureRowIsOnscreen (0);
                safe->list.repaint();
            });
        });
    }

    void BrowserPanel::rebuildKitRows()
    {
        rows.clear();
        const auto tokens = juce::StringArray::fromTokens (search.getText().toLowerCase(), " ", "");
        const auto favourites = library.getFavourites();
        for (int i = 0; i < kitCatalogSize; ++i)
        {
            const auto& e = kitCatalog[i];
            if (category > 0 && category < 9 && categoryForGroup (e.group) != category)
                continue;
            const SampleRef ref { e.root, e.path };
            if (category == 9 && std::find (favourites.begin(), favourites.end(), ref.toString()) == favourites.end())
                continue;
            const auto hay = (juce::String (e.name) + " " + kitGroupNames[(int) e.group] + " " + e.path).toLowerCase();
            bool ok = true;
            for (const auto& t : tokens)
                ok = ok && hay.contains (t);
            if (ok)
                rows.push_back (i);
        }
        list.updateContent();
        list.deselectAllRows();
        list.repaint();
    }

    void BrowserPanel::updateStatus()
    {
        juce::String text;
        if (tab == tabKit)
            text = juce::String ((int) rows.size()) + " OF " + juce::String (kitCatalogSize) + " ZONE KIT SOUNDS";
        else
        {
            // Two lines: counts, then the folder (its end, when the whole path would wrap).
            const auto r = rootForTab (tab);
            const auto folder = library.getRoot (r);
            auto path = folder != juce::File() ? folder.getFullPathName() : juce::String ("NO FOLDER SET");
            if (path.length() > 44)
                path = "..." + path.getLastCharacters (41);
            text = juce::String (rootTag (r)) + "  " + library.getStatusText (r)
                 + (index != nullptr ? "  /  " + juce::String ((int) rows.size()) + " SHOWN" : juce::String()) + "\n" + path;
        }
        if (status.getText() != text)
            status.setText (text, juce::dontSendNotification);
    }

    int BrowserPanel::getNumRows()
    {
        return (int) rows.size();
    }

    SampleRef BrowserPanel::refForRow (int row) const
    {
        if (! juce::isPositiveAndBelow (row, (int) rows.size()))
            return {};
        const int i = rows[(size_t) row];
        if (tab == tabKit)
            return { kitCatalog[i].root, kitCatalog[i].path };
        if (index == nullptr || ! juce::isPositiveAndBelow (i, (int) index->entries.size()))
            return {};
        return index->ref (index->entries[(size_t) i]);
    }

    juce::String BrowserPanel::nameForRow (int row) const
    {
        if (! juce::isPositiveAndBelow (row, (int) rows.size()))
            return {};
        const int i = rows[(size_t) row];
        if (tab == tabKit)
            return kitCatalog[i].name;
        if (index == nullptr || ! juce::isPositiveAndBelow (i, (int) index->entries.size()))
            return {};
        return index->name (index->entries[(size_t) i]);
    }

    void BrowserPanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
    {
        if (! juce::isPositiveAndBelow (row, (int) rows.size()))
            return;
        if (selected)
        {
            g.setColour (Palette::dim.withAlpha (0.35f));
            g.fillRect (0, 0, width, height);
        }
        else if (row % 2 == 1)
        {
            g.setColour (Palette::faint.withAlpha (0.4f));
            g.fillRect (0, 0, width, height);
        }

        const int i = rows[(size_t) row];
        juce::String name, right, tag;
        juce::Colour colour = Palette::phosphor;
        bool favourite = false;
        if (tab == tabKit)
        {
            const auto& e = kitCatalog[i];
            name = e.name;
            right = kitGroupNames[(int) e.group];
            favourite = library.isFavourite ({ e.root, e.path });
        }
        else if (index != nullptr && juce::isPositiveAndBelow (i, (int) index->entries.size()))
        {
            const auto& e = index->entries[(size_t) i];
            name = index->name (e);
            name = name.upToLastOccurrenceOf (".", false, false);
            right = formatLength (e.seconds);
            if (e.bpm > 0.0f)
                right = juce::String (juce::roundToInt (e.bpm)) + " " + right;
            if (e.category != catNone)
                tag = categoryNames[e.category];
            if (e.flags & flagUnsupported)
                colour = Palette::red.withAlpha (0.6f);
        }

        g.setFont (monoFont (11.5f));
        g.setColour (selected ? Palette::bright : colour);
        const int rightWidth = 74;
        g.drawText ((favourite ? "* " : "") + name, 4, 0, width - rightWidth - 8, height, juce::Justification::centredLeft, true);
        g.setFont (monoFont (9.5f));
        g.setColour (Palette::dim);
        g.drawText (right, width - rightWidth - 2, 0, rightWidth, height, juce::Justification::centredRight, true);
        juce::ignoreUnused (tag);
    }

    juce::String BrowserPanel::getTooltipForRow (int row)
    {
        const auto ref = refForRow (row);
        return ref.isEmpty() ? juce::String() : ref.toString();
    }

    void BrowserPanel::previewRow (int row)
    {
        const auto ref = refForRow (row);
        if (! ref.isEmpty())
            ctx.processor.previewSample (ref);
    }

    void BrowserPanel::assignRow (int row)
    {
        const auto ref = refForRow (row);
        const int lane = ctx.selectedLane;
        if (ref.isEmpty() || lane >= laneTime)
            return;
        ctx.processor.assignSample (lane, ref);
    }

    void BrowserPanel::listBoxItemClicked (int row, const juce::MouseEvent&)
    {
        if (previewButton.getToggleState())
            previewRow (row);
    }

    void BrowserPanel::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
    {
        assignRow (row);
    }

    void BrowserPanel::returnKeyPressed (int row)
    {
        assignRow (row);
    }

    juce::var BrowserPanel::getDragSourceDescription (const juce::SparseSet<int>& selected)
    {
        if (selected.isEmpty())
            return {};
        const auto ref = refForRow (selected[0]);
        return ref.isEmpty() ? juce::var() : juce::var ("labx0ref:" + ref.toString());
    }
}
