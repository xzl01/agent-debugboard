use crate::task_error::TaskError;
use crate::task_store::StoreArgs;

pub(crate) const TASK_USAGE: &str =
    "usage: radxa-linkr-debuggerctl task list|store FILE [TASK-ID]|delete TASK-ID|clear|import FILE|export FILE|source TASK-ID FILE|run TASK-ID --confirm";

#[derive(Debug)]
pub(crate) enum TaskCommand {
    Help,
    List,
    Store(StoreArgs),
    Delete { task_id: String },
    Import { path: String },
    Export { path: String },
    Source { task_id: String, path: String },
    Run { task_id: String },
    Clear,
}

pub(crate) fn parse(args: &[String]) -> Result<TaskCommand, TaskError> {
    let cleaned: Vec<&str> = args
        .iter()
        .map(String::as_str)
        .filter(|arg| !matches!(*arg, "--json" | "-v" | "--verbose"))
        .collect();
    if cleaned.first().copied() != Some("task") {
        return usage_error();
    }
    match cleaned.as_slice() {
        ["task", "--help" | "-h"] => Ok(TaskCommand::Help),
        ["task", "list"] => Ok(TaskCommand::List),
        ["task", "store", path] => Ok(TaskCommand::Store(StoreArgs {
            path: (*path).to_string(),
            task_id: None,
        })),
        ["task", "store", path, task_id] => Ok(TaskCommand::Store(StoreArgs {
            path: (*path).to_string(),
            task_id: Some((*task_id).to_string()),
        })),
        ["task", "delete", task_id] => Ok(TaskCommand::Delete {
            task_id: (*task_id).to_string(),
        }),
        ["task", "import", path] => Ok(TaskCommand::Import {
            path: (*path).to_string(),
        }),
        ["task", "export", path] => Ok(TaskCommand::Export {
            path: (*path).to_string(),
        }),
        ["task", "source", task_id, path] => Ok(TaskCommand::Source {
            task_id: (*task_id).to_string(),
            path: (*path).to_string(),
        }),
        ["task", "run", task_id, "--confirm"] | ["task", "run", "--confirm", task_id] => {
            Ok(TaskCommand::Run {
                task_id: (*task_id).to_string(),
            })
        }
        ["task", "run", _] => Err(TaskError::Command {
            code: "confirmation_required",
            message: "task run requires explicit --confirm before any request is sent".to_string(),
        }),
        ["task", "clear"] => Ok(TaskCommand::Clear),
        _ => usage_error(),
    }
}

fn usage_error<T>() -> Result<T, TaskError> {
    Err(TaskError::Command {
        code: "usage",
        message: TASK_USAGE.to_string(),
    })
}

#[cfg(test)]
mod tests {
    use super::{parse, TaskCommand};
    use crate::task_error::TaskError;

    fn args(values: &[&str]) -> Vec<String> {
        values.iter().map(|value| (*value).to_string()).collect()
    }

    #[test]
    fn task_commands_cover_the_complete_surface_and_exclude_boot() {
        assert!(matches!(
            parse(&args(&["task", "--help"])),
            Ok(TaskCommand::Help)
        ));
        assert!(matches!(
            parse(&args(&["task", "list"])),
            Ok(TaskCommand::List)
        ));
        assert!(matches!(
            parse(&args(&["task", "store", "task.ndjson"])),
            Ok(TaskCommand::Store(_))
        ));
        assert!(matches!(
            parse(&args(&["task", "delete", "saved"])),
            Ok(TaskCommand::Delete { task_id }) if task_id == "saved"
        ));
        assert!(matches!(
            parse(&args(&["task", "import", "tasks.ir"])),
            Ok(TaskCommand::Import { path }) if path == "tasks.ir"
        ));
        assert!(matches!(
            parse(&args(&["task", "export", "tasks.ir"])),
            Ok(TaskCommand::Export { path }) if path == "tasks.ir"
        ));
        assert!(matches!(
            parse(&args(&["task", "source", "saved", "saved.linkr-test"])),
            Ok(TaskCommand::Source { task_id, path })
                if task_id == "saved" && path == "saved.linkr-test"
        ));
        assert!(parse(&args(&[
            "task",
            "store-recovery",
            "rockchip-maskrom",
            "12v_out"
        ]))
        .is_err());
        assert!(matches!(
            parse(&args(&["task", "run", "recovery", "--confirm"])),
            Ok(TaskCommand::Run { task_id }) if task_id == "recovery"
        ));
        assert!(matches!(
            parse(&args(&["task", "run", "--confirm", "recovery"])),
            Ok(TaskCommand::Run { task_id }) if task_id == "recovery"
        ));
        assert!(matches!(
            parse(&args(&["task", "run", "recovery"])),
            Err(TaskError::Command {
                code: "confirmation_required",
                ..
            })
        ));
        assert!(matches!(
            parse(&args(&["task", "clear"])),
            Ok(TaskCommand::Clear)
        ));
        assert!(parse(&args(&["task", "boot", "recovery"])).is_err());
        assert!(parse(&args(&["orch", "list"])).is_err());
    }
}
