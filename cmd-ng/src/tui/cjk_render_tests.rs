use super::config_rows::build_saved_config_content;
use super::gpio_fixture::{draw, row_text};
use super::model::{TuiModel, TuiSwitchState};
use super::pages::ActivePage;
use super::status_page::status_lines;
use crate::persistent_config::{ConfigAction, PersistentConfigResponse, PersistentConfigStatus};
use anyhow::{anyhow, Result};
use ratatui::text::Line;
use std::time::Duration;
use unicode_width::UnicodeWidthStr;

const CJK_CONFIG: &str = r#"{"schema":"radxa-linkr-debugger.v1","ok":true,"command":"config","action":"get","backend":{"available":true,"reason":"ready"},"snapshot":{"present":true,"version":1},"pending":0,"items":[{"id":"x配置标识甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉","kind":"switch","current":{"route":"c当前路线甲乙丙丁戊己庚辛壬癸"},"saved":{"route":"s保存路线甲乙丙丁戊己庚辛壬癸"},"selected":true,"requires_confirm":false,"apply_state":"applied"}]}"#;

fn cjk_config_model() -> Result<TuiModel> {
    let response =
        PersistentConfigResponse::from_raw(CJK_CONFIG.to_string()).map_err(anyhow::Error::msg)?;
    response
        .validate(&ConfigAction::Get, None)
        .map_err(anyhow::Error::msg)?;
    let mut model = TuiModel::new("http://test.invalid".to_string(), Duration::from_secs(2));
    model
        .saved_config
        .observe_summary(Some(PersistentConfigStatus {
            available: true,
            reason: "ready".to_string(),
            saved_count: 1,
            pending_count: 0,
        }));
    model
        .saved_config
        .apply_authoritative(response)
        .map_err(anyhow::Error::msg)?;
    model.set_page(ActivePage::SavedConfig);
    Ok(model)
}

fn cjk_status_model() -> TuiModel {
    let mut model = TuiModel::new("http://test.invalid".to_string(), Duration::from_secs(2));
    model.switches.insert(
        "switch".to_string(),
        TuiSwitchState {
            name: "s开关名称甲乙丙丁".to_string(),
            desired_route: "d期望路由甲乙丙丁".to_string(),
            actual_route: "a实际路由甲乙丙丁".to_string(),
            routes: Vec::new(),
            pending_route: None,
            pending_until: None,
            route_intent_active: false,
        },
    );
    model.monitoring.temperature.availability.reason = "温度".repeat(40);
    model.err = Some(format!("x{}", "错误".repeat(40)));
    model
}

fn line_width(line: &Line<'_>) -> usize {
    UnicodeWidthStr::width(line.to_string().as_str())
}

fn span_text<'a>(line: &'a Line<'_>, index: usize) -> Result<&'a str> {
    line.spans
        .get(index)
        .map(|span| span.content.as_ref())
        .ok_or_else(|| anyhow!("span {index} missing from {line:?}"))
}

#[test]
fn saved_config_cjk_columns_fit_narrow_and_full_display_budgets() -> Result<()> {
    // Given
    let mut model = cjk_config_model()?;

    for width in [47usize, 48, 120] {
        model.width = width;

        // When
        let content = build_saved_config_content(&model.saved_config, width);
        let line = content
            .lines
            .first()
            .ok_or_else(|| anyhow!("saved-config row missing at width {width}"))?;

        // Then
        assert_eq!(line_width(line), width, "width={width} line={line:?}");
        if width < 120 {
            let id = span_text(line, 2)?;
            assert_eq!(UnicodeWidthStr::width(id), 20);
            assert!(id.ends_with(' '), "wide ID boundary must be padded: {id:?}");
        } else {
            for (index, budget) in [(2usize, 20usize), (6, 12), (8, 12)] {
                let value = span_text(line, index)?;
                assert_eq!(UnicodeWidthStr::width(value), budget);
                assert!(
                    value.ends_with(' '),
                    "column {index} must pad a dropped wide glyph: {value:?}"
                );
            }
        }

        let buffer = draw(&mut model, width as u16, 12)?;
        assert_eq!(buffer[(7, 4)].symbol(), "配");
        assert!(
            row_text(&buffer, 5).trim().is_empty(),
            "saved-config row wrapped at width {width}"
        );
    }
    Ok(())
}

#[test]
fn status_cjk_columns_monitoring_and_error_stay_within_display_budget() -> Result<()> {
    // Given
    let mut model = cjk_status_model();

    for (width, clipped_width) in [(47usize, 46usize), (48, 48)] {
        // When
        let lines = status_lines(&model, width);
        let switch = lines
            .first()
            .ok_or_else(|| anyhow!("status switch row missing"))?;
        let monitoring = lines
            .iter()
            .find(|line| line.to_string().starts_with("temp:"))
            .ok_or_else(|| anyhow!("temperature monitoring row missing"))?;
        let error = lines
            .last()
            .ok_or_else(|| anyhow!("status error row missing"))?;

        // Then
        assert_eq!(line_width(switch), width, "switch={switch:?}");
        for (index, budget) in [(0usize, 10usize), (2, 12), (4, 12)] {
            let value = span_text(switch, index)?;
            assert_eq!(UnicodeWidthStr::width(value), budget);
            assert!(
                value.ends_with(' '),
                "column {index} must pad a dropped wide glyph: {value:?}"
            );
        }
        assert_eq!(line_width(monitoring), clipped_width);
        assert_eq!(line_width(error), clipped_width);
        assert!(lines.iter().all(|line| line_width(line) <= width));

        model.set_page(ActivePage::Status);
        let buffer = draw(&mut model, width as u16, 12)?;
        assert!(row_text(&buffer, 9).contains("error:"));
        assert!(
            row_text(&buffer, 10).trim().is_empty(),
            "status error row wrapped into the next display row at width {width}"
        );
    }
    Ok(())
}

#[test]
fn top_status_band_cjk_url_action_and_error_obey_47_and_80_columns() -> Result<()> {
    // Given
    let mut narrow = TuiModel::new("http://设备地址/路径".to_string(), Duration::from_secs(2));
    narrow.status = "操作".repeat(40);

    // When
    let narrow_buffer = draw(&mut narrow, 47, 6)?;

    // Then
    let title = row_text(&narrow_buffer, 0);
    assert!(title.contains("Radxa Linkr Debugger TUI"));
    assert!(
        !title.contains("url="),
        "CJK URL must drop first: {title:?}"
    );
    assert_eq!(narrow_buffer[(0, 1)].symbol(), "操");
    assert_eq!(narrow_buffer[(46, 1)].symbol(), " ");
    assert!(row_text(&narrow_buffer, 2).contains("Controls"));

    let mut wide = TuiModel::new("http://设备/路径".to_string(), Duration::from_secs(2));
    wide.status = "维护 mode".to_string();
    let wide_buffer = draw(&mut wide, 80, 6)?;
    assert_eq!(wide_buffer[(36, 0)].symbol(), "设");
    assert_eq!(wide_buffer[(70, 0)].symbol(), " ");
    assert_eq!(wide_buffer[(71, 0)].symbol(), "维");
    assert_eq!(wide_buffer[(73, 0)].symbol(), "护");
    assert_eq!(wide_buffer[(76, 0)].symbol(), "m");
    assert_eq!(wide_buffer[(79, 0)].symbol(), "e");

    wide.err = Some(format!("x{}", "错误".repeat(40)));
    let error_buffer = draw(&mut wide, 80, 6)?;
    assert_eq!(error_buffer[(0, 1)].symbol(), "x");
    assert_eq!(error_buffer[(79, 1)].symbol(), " ");
    assert!(row_text(&error_buffer, 2).contains("Controls"));
    Ok(())
}
