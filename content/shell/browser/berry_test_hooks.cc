// Copyright 2026 SW7FT. All rights reserved.

#include "content/shell/browser/berry_test_hooks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "ui/aura/event_injector.h"
#include "ui/aura/window_tree_host.h"
#include "ui/events/event.h"
#include "ui/gfx/geometry/point.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_input_callback.h"

namespace content {

namespace {

aura::WindowTreeHost* g_host = nullptr;

void Log(const char* what, const std::string& detail) {
  fprintf(stderr, "BerryTest: %s %s\n", what, detail.c_str());
}

// Mirrors QnxScreenEventSource::ProcessTouchEvent: the toolbar is offered the
// touch first, then it enters Aura as a touch event from the platform.
void InjectTouch(ui::EventType type, int x, int y) {
  if (!g_host)
    return;
  if (auto toolbar = ui::GetQnxScreenTouchCallback()) {
    if (toolbar(type == ui::ET_TOUCH_PRESSED ? 0 : 2, x, y)) {
      Log("touch taken by toolbar", "");
      return;
    }
  }
  ui::PointerDetails details(ui::EventPointerType::kTouch, /*pointer_id=*/0,
                             1.0f, 1.0f, 0.0f);
  ui::TouchEvent touch(type, gfx::Point(x, y), base::TimeTicks::Now(),
                       details);
  aura::EventInjector().Inject(g_host, &touch);
}

void RunCommand(const std::string& line) {
  int x = 0, y = 0, hold_ms = 0;
  if (sscanf(line.c_str(), "touch %d %d %d", &x, &y, &hold_ms) == 3) {
    Log("run", line);
    InjectTouch(ui::ET_TOUCH_PRESSED, x, y);
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(&InjectTouch, ui::ET_TOUCH_RELEASED, x, y),
        base::Milliseconds(hold_ms));
    return;
  }
  if (line.compare(0, 7, "invoke ") == 0) {
    std::string text;
    for (size_t i = 7; i < line.size(); ++i) {
      if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == 'n') {
        text += '\n';
        ++i;
      } else {
        text += line[i];
      }
    }
    Log("run", "invoke");
    if (auto invoke = ui::GetQnxScreenInvokeUriCallback())
      invoke(text.c_str());
    return;
  }
  Log("unknown command", line);
}

void Poll(const std::string& path) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f)
    return;
  std::string content;
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    content.append(buf, n);
  fclose(f);
  unlink(path.c_str());

  size_t start = 0;
  while (start < content.size()) {
    size_t end = content.find('\n', start);
    if (end == std::string::npos)
      end = content.size();
    if (end > start)
      RunCommand(content.substr(start, end - start));
    start = end + 1;
  }
}

}  // namespace

void BerryStartTestHooks(aura::WindowTreeHost* host) {
  const char* path = getenv("BERRY_TEST_CMD");
  if (!path || !path[0])
    return;
  g_host = host;
  static base::NoDestructor<base::RepeatingTimer> timer;
  timer->Start(FROM_HERE, base::Milliseconds(50),
               base::BindRepeating(&Poll, std::string(path)));
  Log("hooks on", path);
}

}  // namespace content
