// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

#[wasm_bindgen::prelude::wasm_bindgen(js_name = decodeLogic)]
pub fn decode_logic_wasm(request_json: &str) -> String {
    crate::decode_json(request_json)
}
