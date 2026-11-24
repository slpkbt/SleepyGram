# Screen reader support for reactions and the emoji panel

The client paints both surfaces with `QPainter` and never creates a child
widget per element, so the accessibility tree has nothing to report: NVDA
reaches the popup, hears nothing inside it, and the emoji grid is a single
anonymous pane. This task gives both surfaces an explicit accessible model
that lives entirely in `Telegram/SourceFiles`, without touching `lib_ui` and
without any forked library.

## Invariant

Every element the user can select with the keyboard is reachable by a screen
reader through the same model the keyboard walks: the accessible child count
equals the number of selectable elements, the child name equals the text the
element shows, the child rectangle equals the element's painted rectangle, and
`SetFocus`/`Activate` act on the element the assistive technology named, not
on whatever sits at that index later.

## Decisions

- **No `lib_ui` changes and no library forks.** The virtual child API already
  exists in `Telegram/lib_ui/ui/rp_widget.h:381-504` (`accessibilityChildCount`,
  `ChildName`, `ChildRole`, `ChildRect`, `ChildState`, `ChildIdentity`,
  `IndexByIdentity`, `ChildSupportsActions`, `ChildSetFocus`, `ChildActivate`),
  so an application-level widget can describe itself completely.
- **Announcement lives in the application layer.** `lib_ui` has no
  `accessibilityAnnounce` (it is only proposed in `desktop-app/lib_ui#377`),
  and the pinned library must stay upstream. A single application module
  `Telegram/SourceFiles/ayu/ui/ayu_screen_reader.{h,cpp}` owns the call, so
  when upstream ships the same helper we replace one file instead of every
  call site. Qt here is 6.11.2 (`.github/workflows/build.yml:44`), so
  `QAccessibleAnnouncementEvent` is available; older Qt compiles to a no-op.
- **Position in the set is part of the name.** `lib_ui` has no
  position-in-set virtual, so the name carries the localized "N of M" suffix,
  the same text a screen reader would otherwise have to infer.

## Surfaces

### Reactions panel

`HistoryView::Reactions::Selector` (`history/view/reactions/history_view_reactions_selector.cpp`)
is a painted `Ui::RpWidget` inside a `Ui::PopupMenu`. NVDA reads only menu
items inside a `Qt::Popup` window, so the panel must leave the popup: it opens
as its own `Ui::BoxContent` layer with real focus, the way Unigram's
`ReactionsMenuFlyout` does (`Telegram/Controls/Messages/ReactionButton.cs`).

### Emoji panel

`ChatHelpers::EmojiListWidget` (`chat_helpers/emoji_list_widget.cpp`) already
tracks the element under the cursor as `OverEmoji`; the accessible child list
is built from the same `lookupOverEmoji`/`overEmoji` helpers that painting and
hover use, so the two models cannot drift.

## Verification

- NVDA speech log per surface, recorded in the pull request: open, move with
  arrows, activate, dismiss.
- A `-testagent` scenario under `Telegram/SourceFiles/test/` that queries
  `QAccessible::queryAccessibleInterface(widget)`, checks child count against
  the model, and reads back each child name and rectangle.
- `python3 tools/rules/hook.py --staged` and the repository CI build.
