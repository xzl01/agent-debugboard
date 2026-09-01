use super::IconPublisher;
use std::os::unix::fs::PermissionsExt;

#[test]
fn current_icon_slot_is_protected_before_updates() {
    let root =
        std::env::temp_dir().join(format!("linkr-icon-publisher-test-{}", std::process::id()));
    let publisher = IconPublisher::new(&root).unwrap();

    publisher.protect_initial().unwrap();

    let current = std::fs::metadata(&publisher.slots[0])
        .unwrap()
        .permissions()
        .mode();
    assert_eq!(current & 0o222, 0);
    for slot in &publisher.slots[1..] {
        let mode = std::fs::metadata(slot).unwrap().permissions().mode();
        assert_ne!(mode & 0o200, 0);
    }
    std::fs::set_permissions(&publisher.slots[0], std::fs::Permissions::from_mode(0o700)).unwrap();
    std::fs::remove_dir_all(root).unwrap();
}
