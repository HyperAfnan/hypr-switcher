# Alt-Tab Mapping Issue Report

## Issue Summary

When `alt-tab` is mapped to `hyprswitcher` in Hyprland, the initial keypress opens the overlay window correctly. However, subsequent `alt-tab` presses spawn **new instances** of hyprswitcher instead of cycling through the client entries within the existing overlay.

---

## Table of Contents

1. [Reproduction Steps](#reproduction-steps)
2. [Technical Analysis](#technical-analysis)
3. [Root Causes](#root-causes)
4. [Proposed Solutions](#proposed-solutions)
5. [Recommended Implementation](#recommended-implementation)

---

## Reproduction Steps

### Prerequisites
- Hyprland compositor running
- hyprswitcher installed
- Multiple windows open

### Steps to Reproduce

1. Add the following keybinding to your Hyprland config (`~/.config/hypr/hyprland.conf`):
   ```conf
   bind = ALT, TAB, exec, hyprswitcher
   ```

2. Open 2+ windows/applications

3. Press `Alt+Tab` to invoke hyprswitcher:
   - **Expected**: Overlay opens showing window list
   - **Actual**: Overlay opens correctly ✓

4. While holding `Alt`, press `Tab` again:
   - **Expected**: Selection cycles to the next window in the list
   - **Actual**: A **new instance** of hyprswitcher is spawned (new overlay process)

5. Release `Alt`:
   - **Expected**: Focus switches to selected window
   - **Actual**: Behavior is unpredictable due to multiple instances

---

## Technical Analysis

### How Hyprland Keybindings Work

When you configure:
```conf
bind = ALT, TAB, exec, hyprswitcher
```

Hyprland interprets this as: "When `Alt` is held AND `Tab` is pressed, execute `hyprswitcher`".

**Key observation**: This binding is evaluated at the **compositor level**, meaning:
- Every `Tab` press while `Alt` is held triggers the `exec` command
- The compositor has no knowledge that hyprswitcher is already running
- There's no built-in mechanism to prevent multiple invocations

### How hyprswitcher Handles Keyboard Input

From analyzing `src/input.c`:

```c
// input.c lines 172-178
if (is_tab) {
    update_mods_from_state();
    LOG_DEBUG("[INPUT] Tab pressed (sym=%u focus=%d alt_down=%d)", sym, g_has_focus, g_alt_down);
    if (g_alt_down) {
        g_alt_tab_flag = true;
        LOG_DEBUG("[INPUT] Alt+Tab chord detected (sym=%u focus=%d)", sym, g_has_focus);
    }
}
```

And from `src/wayland.c`:

```c
// wayland.c lines 232-238
if (input_alt_tab_triggered()) {
    if (g_client_count > 0 && g_titles) {
        g_selection_index = (g_selection_index + 1) % (int)g_client_count;
        LOG_DEBUG("[INPUT] Alt+Tab triggered; new selection index: %d", g_selection_index);
        render_draw_titles_focus(surface, current_width, current_height, g_titles, g_client_count, g_selection_index);
    }
}
```

**Finding**: hyprswitcher **does** have internal logic to handle `Alt+Tab` cycling. The code correctly:
1. Detects Tab key presses
2. Checks if Alt modifier is held
3. Cycles the selection index
4. Redraws the overlay with updated selection

### The Keyboard Focus Problem

The critical issue is **keyboard focus ownership**:

1. **Initial State**: Hyprland has keyboard focus
2. **First Alt+Tab**: hyprswitcher spawns, creates layer surface, requests keyboard focus
3. **Focus Transfer**: Layer shell requests keyboard interactivity (line 271-272 in input.c):
   ```c
   void input_enable_layer_keyboard(struct zwlr_layer_surface_v1 *layer_surface) {
       zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 1);
   }
   ```

4. **Race Condition**: Between:
   - hyprswitcher requesting focus
   - Wayland compositor granting focus
   - Next Tab keypress occurring

If the next `Tab` press happens **before** hyprswitcher gains keyboard focus, Hyprland still processes it and spawns another instance.

### Keyboard Interactivity Modes

The wlr-layer-shell protocol defines keyboard interactivity levels:
- `NONE` (0): No keyboard events
- `EXCLUSIVE` (1): Layer surface gets all keyboard events
- `ON_DEMAND` (2): Gets focus when clicked

hyprswitcher uses mode `1` (exclusive), but there's still a timing window issue.

---

## Root Causes

### Primary Cause: Compositor-Level Binding Execution

**Problem**: The `bind = ALT, TAB, exec, hyprswitcher` binding is evaluated at the Hyprland compositor level, regardless of whether hyprswitcher already has keyboard focus.

**Consequence**: Each Tab press while Alt is held triggers the exec command before the Wayland event reaches hyprswitcher's client-side handler.

### Secondary Cause: Single Instance Protection Missing

**Problem**: hyprswitcher has no mechanism to:
1. Check if an instance is already running
2. Signal an existing instance to cycle
3. Lock to prevent multiple spawns

### Tertiary Cause: Focus Timing Race

**Problem**: The focus handoff between compositor and layer shell may not complete before subsequent keypresses arrive.

---

## Proposed Solutions

### Solution 1: Singleton Pattern with Lock File (Recommended)

**Description**: Implement a lock file mechanism to ensure only one instance runs.

**Implementation**:

```c
// Add to main.c or new singleton.c

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

// NOTE: Use XDG_RUNTIME_DIR in production for security (avoids /tmp symlink attacks)
// Example: snprintf(lock_path, sizeof(lock_path), "%s/hyprswitcher.lock", getenv("XDG_RUNTIME_DIR"));
#define LOCK_FILE "/tmp/hyprswitcher.lock"

static int g_lock_fd = -1;

static int acquire_instance_lock(void) {
    g_lock_fd = open(LOCK_FILE, O_CREAT | O_RDWR, 0600);
    if (g_lock_fd < 0) {
        return -1;
    }
    
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) < 0) {
        // Another instance is running - could send a signal to cycle
        close(g_lock_fd);
        g_lock_fd = -1;
        return -1;
    }
    
    return 0;
}

static void release_instance_lock(void) {
    if (g_lock_fd >= 0) {
        flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        unlink(LOCK_FILE);
        g_lock_fd = -1;
    }
}
```

**In main()**:
```c
int main() {
    // Register cleanup for abnormal exits
    atexit(release_instance_lock);
    signal(SIGINT, exit);
    signal(SIGTERM, exit);
    
    if (acquire_instance_lock() != 0) {
        // Instance already running
        // Option A: Exit silently
        return 0;
        // Option B: Send signal to existing instance to cycle
    }
    
    // ... existing code ...
    
    release_instance_lock();
    return 0;
}
```

**Pros**:
- Simple to implement
- Prevents resource waste from multiple instances
- Works with existing Hyprland binding

**Cons**:
- Silently ignoring subsequent invocations doesn't cycle - need IPC for that

---

### Solution 2: IPC-Based Cycling Command (Most Complete)

**Description**: Allow a second invocation to send a "cycle" command to the running instance via Unix socket or signal.

**Implementation Outline**:

1. Create a Unix socket on startup: `/tmp/hyprswitcher.sock`
2. Listen for "CYCLE" commands
3. When a new instance starts and detects existing socket:
   - Send "CYCLE" command
   - Exit immediately
4. Existing instance receives command and cycles selection

```c
// Pseudo-code for IPC approach

// In existing instance's event loop:
void handle_ipc_command(const char *cmd) {
    if (strcmp(cmd, "CYCLE") == 0) {
        // Trigger the same logic as internal Alt+Tab
        g_selection_index = (g_selection_index + 1) % g_client_count;
        render_draw_titles_focus(...);
    }
}

// In new instance's startup:
if (socket_exists("/tmp/hyprswitcher.sock")) {
    send_command("CYCLE");
    exit(0);
}
```

**Pros**:
- Full functionality: each Alt+Tab cycles the selection
- Clean architecture
- Can extend for other commands (cycle backward, close, etc.)

**Cons**:
- More complex implementation
- Requires adding event loop monitoring for IPC socket

---

### Solution 3: Alternative Hyprland Binding Configuration

**Description**: Use a different keybinding strategy that doesn't repeatedly exec hyprswitcher.

**Option A: Use SUPER+Tab for initial invoke, Alt+Tab handled internally**
```conf
bind = SUPER, TAB, exec, hyprswitcher
# Alt+Tab is then handled by hyprswitcher when it has focus
```

**Option B: Use a submap for Alt+Tab handling**
```conf
# Enter switcher mode
bind = ALT, TAB, exec, hyprswitcher
bind = ALT, TAB, submap, switcher

# Switcher submap - Tab does nothing at compositor level
submap = switcher
bind = , TAB, pass,
bind = , ESCAPE, submap, reset
bindrt = ALT, ALT_L, submap, reset
bindrt = ALT, ALT_R, submap, reset
submap = reset
```

This approach:
1. First Alt+Tab starts hyprswitcher and enters "switcher" submap
2. In switcher submap, Tab key is passed through to focused surface
3. hyprswitcher receives Tab events directly
4. Releasing Alt (or pressing Escape) exits the submap

**Option C: Use pass keyword (Hyprland v0.40+)**
```conf
bindn = ALT, TAB, pass, class:^(hyprswitcher)$
bind = ALT, TAB, exec, hyprswitcher
```

**Pros**:
- Works with existing code
- No code changes required (Option A, B, C)

**Cons**:
- Option A: Changes expected user experience
- Options B/C: Complex configuration, may not work on all Hyprland versions

---

### Solution 4: Signal-Based Cycling (Simpler IPC)

**Description**: Use Unix signals for inter-process communication.

**Implementation**:

```c
// In main.c

#include <signal.h>

// NOTE: Use XDG_RUNTIME_DIR in production for security
// Example: snprintf(pid_path, sizeof(pid_path), "%s/hyprswitcher.pid", getenv("XDG_RUNTIME_DIR"));
#define PID_FILE "/tmp/hyprswitcher.pid"

static volatile sig_atomic_t g_cycle_requested = 0;

void handle_sigusr1(int sig) {
    (void)sig;
    g_cycle_requested = 1;
}

int main() {
    // Check for existing instance via pidfile
    pid_t existing_pid = read_pidfile(PID_FILE);
    if (existing_pid > 0 && kill(existing_pid, 0) == 0) {
        // Instance exists, send signal to cycle
        kill(existing_pid, SIGUSR1);
        return 0;
    }
    
    // Write our PID
    write_pidfile(PID_FILE, getpid());
    
    // Setup signal handler
    signal(SIGUSR1, handle_sigusr1);
    
    // ... existing code ...
}

// In wayland_loop():
if (g_cycle_requested) {
    g_cycle_requested = 0;
    g_selection_index = (g_selection_index + 1) % g_client_count;
    render_draw_titles_focus(...);
}
```

**Pros**:
- Simpler than socket-based IPC
- Full cycling functionality
- Minimal code additions

**Cons**:
- Signals are async and can interrupt system calls
- PID file management required

---

## Recommended Implementation

### Short-Term Fix: Solution 1 (Lock File)

For an immediate fix that prevents the worst symptom (multiple instances):

1. Add singleton lock in `main.c`
2. Exit gracefully if instance already running

This prevents resource exhaustion and window stacking issues.

### Long-Term Fix: Solution 4 (Signal-Based) or Solution 2 (Socket IPC)

For complete functionality where each `Alt+Tab` press cycles:

1. Implement PID file tracking
2. Add signal handler (SIGUSR1) or socket listener
3. New invocations trigger the cycle in existing instance

### Documentation Fix: Update README with Better Config

Add to README.md:

```markdown
## Recommended Hyprland Configuration

For the best Alt+Tab experience, use one of these configurations:

### Option 1: Simple (SUPER+Tab)
```conf
bind = SUPER, TAB, exec, hyprswitcher
```
Initial invoke with Super+Tab, then Alt+Tab cycles within overlay.

### Option 2: Traditional Alt+Tab (requires singleton/IPC implementation)
```conf
bind = ALT, TAB, exec, hyprswitcher
```
Requires hyprswitcher with singleton/IPC support to handle multiple invocations (see proposed solutions above).

### Option 3: Submap Approach
```conf
bind = ALT, TAB, exec, hyprswitcher
bind = ALT, TAB, submap, switcher
submap = switcher
bind = , TAB, pass,
bindrt = ALT, ALT_L, submap, reset
bindrt = ALT, ALT_R, submap, reset
submap = reset
```

---

## Summary

| Solution | Complexity | Cycling Works | Recommended For |
|----------|------------|---------------|-----------------|
| Lock File (1) | Low | No | Quick fix |
| Socket IPC (2) | High | Yes | Production use |
| Alt Config (3) | None | Partial | Users who can adapt |
| Signal IPC (4) | Medium | Yes | Balance of effort/features |

The issue fundamentally stems from Hyprland's binding system executing the `exec` command before the overlay can capture keyboard focus. The recommended path forward is implementing **Solution 4 (Signal-Based IPC)** as it provides full functionality with reasonable complexity.

---

## References

- hyprswitcher source: `src/input.c`, `src/wayland.c`, `src/main.c`
- Hyprland wiki: https://wiki.hyprland.org/Configuring/Binds/
- wlr-layer-shell protocol: `protocols/wlr-layer-shell-unstable-v1.xml`
- xkbcommon: https://xkbcommon.org/

---

*Report generated: Analysis of GitHub issue "Mapping in hyprland with alt-tab does not work properly"*
