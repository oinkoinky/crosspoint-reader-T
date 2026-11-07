#include "LinkActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "activities/reader/ReaderUtils.h"
#include "fontIds.h"
#include "util/UnitLinkService.h"

void LinkActivity::onEnter() {
  Activity::onEnter();
  if (!HalLink::isSupported()) {
    finish();
    return;
  }
  if (UnitLinkService::get().running()) {  // the link was started earlier and kept running
    role = UnitLinkService::get().role();
    session = &UnitLinkService::get().linkSession();
    state = State::Running;
    requestUpdate();
    return;
  }
  state = State::ChooseSide;
  const StrId options[] = {StrId::STR_LINK_LEFT_PAGE, StrId::STR_LINK_RIGHT_PAGE, StrId::STR_CANCEL};
  sidePopup.show(StrId::STR_LINK_UNIT, options, 3, 0, [this](int idx) {
    if (idx == 0) {
      startLink(unitlink::Role::Left);
    } else if (idx == 1) {
      startLink(unitlink::Role::Right);
    } else {
      finish();
    }
  });
  requestUpdate();
}

void LinkActivity::onExit() {
  // Leaving through finish() before a side was chosen needs nothing; once the link runs, endLink() restarts the unit.
  Activity::onExit();
}

void LinkActivity::startLink(unitlink::Role chosen) {
  role = chosen;
  auto& service = UnitLinkService::get();
  if (!service.start(chosen, renderer)) {
    LOG_ERR("LINK", "Link could not start");
    finish();
    return;
  }
  session = &service.linkSession();
  state = State::Running;
  requestUpdate();
}

void LinkActivity::endLink() { UnitLinkService::get().endAndRestart(); }

void LinkActivity::loop() {
  if (state == State::ChooseSide) {
    if (sidePopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  // The reader's own page buttons: the side buttons and the front Left/Right buttons.
  const auto turn = ReaderUtils::detectPageTurn(mappedInput);
  if (role == unitlink::Role::Left) {
    // Test page: stands in for the reader's page until the reader drives the link. Either unit can turn it: this
    // unit's buttons, and the right unit's, which arrive as keys.
    int delta = (turn.next ? 1 : 0) - (turn.prev ? 1 : 0);
    unitlink::KeyAction key;
    while (session->takeKey(key)) {
      if (key == unitlink::KeyAction::NextPage) delta++;
      if (key == unitlink::KeyAction::PrevPage) delta--;
    }
    if (delta != 0) {
      testPage = static_cast<decltype(testPage)>(std::max(0, static_cast<int>(testPage) + delta));
      testPageKnown = true;
      session->publish(0, testPage, millis());
    }
  } else {
    // The right unit follows the left unit's page; its buttons ask the left unit to turn.
    if (turn.next) {
      session->sendKey(unitlink::KeyAction::NextPage, millis());
    } else if (turn.prev) {
      session->sendKey(unitlink::KeyAction::PrevPage, millis());
    }
    unitlink::PageState received;
    if (session->takeRemote(received)) {
      testPage = received.page;
      testPageKnown = true;
    }
  }
  if (session->takeSleepRequest()) LOG_INF("LINK", "Peer asked for sleep (not acted on yet)");

  const bool acked = session->publishedIsAcked();
  const bool countersDue = millis() - lastCountersDrawMs >= 2000;
  if (session->status() != shownStatus || testPage != shownPage || acked != shownAcked || countersDue) {
    lastCountersDrawMs = millis();
    requestUpdate();
  }

  // Act on the release, not the press: the Settings list opens its highlighted row when Confirm is released, so
  // leaving on the press would let that release re-open this screen straight away.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    endLink();
  }
}

void LinkActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LINK_UNIT));

  if (state == State::ChooseSide) {
    if (sidePopup.processRender(renderer, mappedInput)) return;
    renderer.displayBuffer();
    return;
  }

  shownStatus = session->status();
  shownPage = testPage;
  shownAcked = session->publishedIsAcked();

  const char* statusText = I18N.get(UnitLinkService::get().statusMessage());

  const int mid = pageHeight / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, mid - 70,
                            role == unitlink::Role::Left ? tr(STR_LINK_LEFT_PAGE) : tr(STR_LINK_RIGHT_PAGE), true,
                            EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, mid - 30, statusText);
  if (shownStatus == unitlink::Status::Linked || shownStatus == unitlink::Status::Lost) {
    char line[48];
    if (role == unitlink::Role::Left || testPageKnown) {
      snprintf(line, sizeof(line), "%u%s", static_cast<unsigned>(testPage), shownAcked ? "" : " ...");
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 10, line, true, EpdFontFamily::BOLD);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, mid + 40,
                              role == unitlink::Role::Left ? tr(STR_LINK_TEST_LEFT) : tr(STR_LINK_TEST_RIGHT));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, mid + 90, tr(STR_LINK_END_HINT));
  {
    auto& service = UnitLinkService::get();
    char counters[64];
    snprintf(counters, sizeof(counters), "%u / %u, %u, %u", static_cast<unsigned>(service.bytesSent()),
             static_cast<unsigned>(service.bytesReceived()), static_cast<unsigned>(session->framesReceived()),
             static_cast<unsigned>(session->frameErrors()));
    renderer.drawCenteredText(SMALL_FONT_ID, mid + 140, tr(STR_LINK_COUNTERS));
    renderer.drawCenteredText(SMALL_FONT_ID, mid + 165, counters);
    if (service.signalDbm() != 0) {
      char signal[40];
      snprintf(signal, sizeof(signal), "%s %d", tr(STR_LINK_SIGNAL), service.signalDbm());
      renderer.drawCenteredText(SMALL_FONT_ID, mid + 190, signal);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_LINK_END), tr(STR_LINK_KEEP), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
