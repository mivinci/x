## ADDED Requirements

### Requirement: Format field on task configuration
The task configuration struct SHALL include a format field that selects the streaming protocol and scheduler.

#### Scenario: Default format (backwards compatibility)
- **WHEN** `dlp_task_conf_t.format` is not set (0)
- **THEN** `dlp_task_create` SHALL select the MP4 scheduler (`dlp_sched_mp4`), preserving existing behavior

#### Scenario: HLS format
- **WHEN** `dlp_task_conf_t.format` is set to `DLP_FMT_HLS`
- **THEN** `dlp_task_create` SHALL select the HLS scheduler (`dlp_sched_hls`)

### Requirement: Scheduler selection by format
`dlp_task_create` SHALL select the scheduler implementation based on the task's format field.

#### Scenario: MP4 task creation
- **WHEN** a task is created with `format = DLP_FMT_MP4`
- **THEN** the task's `sched` pointer SHALL be set to `&dlp_sched_mp4`

#### Scenario: HLS task creation
- **WHEN** a task is created with `format = DLP_FMT_HLS`
- **THEN** the task's `sched` pointer SHALL be set to `&dlp_sched_hls`

### Requirement: HLS fields on task struct
The internal task struct SHALL carry HLS-specific state when the task is in HLS mode.

#### Scenario: HLS task fields
- **WHEN** a task is created with `format = DLP_FMT_HLS`
- **THEN** the task struct SHALL provide storage for: segment list (array of segment records), segment count, current player segment index, playlist URL, and the index of the segment currently being downloaded
