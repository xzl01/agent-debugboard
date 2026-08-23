pub const DEFAULT_BASE_URL: &str = "http://172.29.203.1";

pub fn resolve_base_url(input: &str) -> String {
    let trimmed = input.trim();
    if trimmed.is_empty() {
        return DEFAULT_BASE_URL.to_string();
    }
    if trimmed.starts_with("http://") || trimmed.starts_with("https://") {
        return trimmed.trim_end_matches('/').to_string();
    }
    format!("http://{}", trimmed.trim_end_matches('/'))
}

pub(crate) fn join_request_path(base_path: &str, request_path: &str) -> String {
    let base = base_path.trim_end_matches('/');
    let req = request_path.trim_start_matches('/');
    if base.is_empty() {
        return format!("/{req}");
    }
    format!("{base}/{req}")
}

#[cfg(test)]
mod tests {
    use super::{join_request_path, resolve_base_url, DEFAULT_BASE_URL};

    #[test]
    fn uses_default_url() {
        assert_eq!(resolve_base_url(""), DEFAULT_BASE_URL);
    }

    #[test]
    fn normalizes_host_port() {
        assert_eq!(
            resolve_base_url("172.29.203.1:9090/"),
            "http://172.29.203.1:9090"
        );
    }

    #[test]
    fn joins_request_path_like_go() {
        assert_eq!(
            join_request_path("/prefix", "/api/v1/status"),
            "/prefix/api/v1/status"
        );
        assert_eq!(join_request_path("", "/api/v1/status"), "/api/v1/status");
    }
}
