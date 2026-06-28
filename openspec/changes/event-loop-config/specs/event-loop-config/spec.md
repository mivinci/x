# event-loop-config

## ADDED Requirements

### Requirement: Event loop creation with configuration

The system SHALL provide `xEventLoopCreateWithConf(const xEventLoopConf *conf)` that creates an event loop configured via a struct with `group` (default offload task group) and `name` (thread label) fields.

- When `conf` is NULL, the system SHALL create a loop with no task group and no name.
- When `group` is set, the system SHALL use it as the default task group for `xWorkSubmit()`.
- When `name` is set, the system SHALL copy it (truncated to 15 characters) into the loop's internal state.

#### Scenario: Create with defaults
- **WHEN** an event loop is created with `xEventLoopCreateWithConf(NULL)`
- **THEN** the system returns a valid `xEventLoop` handle with no task group and an empty name

#### Scenario: Create with group and name
- **WHEN** an event loop is created with `xEventLoopConf{ .group = group, .name = "http-srv" }`
- **THEN** the loop uses `group` as its default task group and stores `"http-srv"` as its name

#### Scenario: Long name truncated
- **WHEN** an event loop is created with a name longer than 15 characters
- **THEN** the system truncates the name to 15 characters and NUL-terminates it

### Requirement: Backward-compatible creation functions

The system SHALL retain `xEventLoopCreate()` and `xEventLoopCreateWithGroup(xTaskGroup)` as convenience wrappers that delegate to `xEventLoopCreateWithConf`.

- `xEventLoopCreate()` SHALL be equivalent to `xEventLoopCreateWithConf(NULL)`.
- `xEventLoopCreateWithGroup(group)` SHALL be equivalent to `xEventLoopCreateWithConf(&(xEventLoopConf){ .group = group })`.
- Existing callers SHALL compile and behave identically without modification.

#### Scenario: Existing xEventLoopCreate unchanged
- **WHEN** code calls `xEventLoopCreate()`
- **THEN** the system creates a loop with no group and no name, identical to current behavior

#### Scenario: Existing xEventLoopCreateWithGroup unchanged
- **WHEN** code calls `xEventLoopCreateWithGroup(group)`
- **THEN** the system creates a loop with the specified group, identical to current behavior

### Requirement: Thread naming on Enter

The system SHALL set the calling thread's OS name to the loop's configured name when `xEventLoopEnter` is called and the loop has a non-empty name.

- When the loop's name is empty, the system SHALL NOT modify the thread name.
- Thread naming SHALL NOT be undone on `xEventLoopLeave` — the thread retains the last-set name.

#### Scenario: Named loop sets thread name
- **WHEN** a loop with name `"my-worker"` is entered via `xEventLoopEnter`
- **THEN** the current thread's OS name is set to `"my-worker"`

#### Scenario: Unnamed loop does not change thread name
- **WHEN** a loop with an empty name is entered via `xEventLoopEnter`
- **THEN** the current thread's OS name is not modified

#### Scenario: Name persists after Leave
- **WHEN** a named loop is entered and then left via `xEventLoopLeave`
- **THEN** the thread name remains the loop's name (it is not cleared or restored)
