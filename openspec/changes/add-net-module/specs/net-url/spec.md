## ADDED Requirements

### Requirement: Url parse
`Url::parse(raw)` returns `Result<Url, xErrno>`. Wraps `xUrlParse`.

#### Scenario: Parse valid URL
- **WHEN** `Url::parse("https://example.com:8080/path?q=1")` is called
- **THEN** the Result is Ok, with `scheme() == "https"`, `host() == "example.com"`, `port() == 8080`, `path() == "/path"`

#### Scenario: Parse invalid URL
- **WHEN** `Url::parse("not a url")` is called
- **THEN** the Result is Err

### Requirement: Url RAII
`~Url()` calls `xUrlFree`. Move-only.

#### Scenario: Destructor frees
- **WHEN** a Url goes out of scope
- **THEN** `xUrlFree` is called, internal string copy released

### Requirement: Url accessors
`scheme()`, `host()`, `port()`, `path()` return parsed components.

#### Scenario: Access components
- **WHEN** `Url::parse("http://localhost:3000/api")` succeeds
- **THEN** `scheme() == "http"`, `host() == "localhost"`, `port() == 3000`, `path() == "/api"`
