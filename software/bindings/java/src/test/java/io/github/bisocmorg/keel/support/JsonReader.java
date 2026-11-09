// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.support;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * A small reader for the conformance scenario file, so the tests need no JSON library: objects become ordered
 * maps, arrays lists, numbers {@link Long} (or {@link Double} with a fraction or exponent), and the literals
 * {@code true}, {@code false} and {@code null} their Java values.
 */
public final class JsonReader {
    private final String text;
    private int at;

    private JsonReader(String text) {
        this.text = text;
    }

    /** Parses one JSON document. */
    public static Object parse(String text) {
        JsonReader reader = new JsonReader(text);
        Object value = reader.value();
        reader.space();
        if (reader.at != text.length()) {
            throw reader.error("text after the document");
        }
        return value;
    }

    private Object value() {
        space();
        if (at >= text.length()) {
            throw error("unexpected end");
        }
        char next = text.charAt(at);
        return switch (next) {
            case '{' -> object();
            case '[' -> array();
            case '"' -> string();
            case 't' -> literal("true", Boolean.TRUE);
            case 'f' -> literal("false", Boolean.FALSE);
            case 'n' -> literal("null", null);
            default -> number();
        };
    }

    private Map<String, Object> object() {
        Map<String, Object> result = new LinkedHashMap<>();
        at++;
        space();
        if (peek('}')) {
            return result;
        }
        do {
            space();
            String key = string();
            space();
            expect(':');
            result.put(key, value());
            space();
        } while (peek(','));
        expect('}');
        return result;
    }

    private List<Object> array() {
        List<Object> result = new ArrayList<>();
        at++;
        space();
        if (peek(']')) {
            return result;
        }
        do {
            result.add(value());
            space();
        } while (peek(','));
        expect(']');
        return result;
    }

    private String string() {
        expect('"');
        StringBuilder result = new StringBuilder();
        while (true) {
            if (at >= text.length()) {
                throw error("unterminated string");
            }
            char next = text.charAt(at++);
            if (next == '"') {
                return result.toString();
            }
            if (next != '\\') {
                result.append(next);
                continue;
            }
            char escaped = text.charAt(at++);
            switch (escaped) {
                case '"', '\\', '/' -> result.append(escaped);
                case 'b' -> result.append('\b');
                case 'f' -> result.append('\f');
                case 'n' -> result.append('\n');
                case 'r' -> result.append('\r');
                case 't' -> result.append('\t');
                case 'u' -> {
                    result.append((char) Integer.parseInt(text.substring(at, at + 4), 16));
                    at += 4;
                }
                default -> throw error("bad escape \\" + escaped);
            }
        }
    }

    private Object number() {
        int start = at;
        while (at < text.length() && "+-0123456789.eE".indexOf(text.charAt(at)) >= 0) {
            at++;
        }
        String digits = text.substring(start, at);
        if (digits.isEmpty()) {
            throw error("unexpected character");
        }
        // Not a conditional expression: Long and Double operands would both be promoted to double.
        if (digits.matches("-?\\d+")) {
            return Long.valueOf(digits);
        }
        return Double.valueOf(digits);
    }

    private Object literal(String word, Object value) {
        if (!text.startsWith(word, at)) {
            throw error("expected " + word);
        }
        at += word.length();
        return value;
    }

    private void space() {
        while (at < text.length() && Character.isWhitespace(text.charAt(at))) {
            at++;
        }
    }

    private boolean peek(char wanted) {
        if (at < text.length() && text.charAt(at) == wanted) {
            at++;
            return true;
        }
        return false;
    }

    private void expect(char wanted) {
        if (!peek(wanted)) {
            throw error("expected '" + wanted + "'");
        }
    }

    private IllegalArgumentException error(String what) {
        return new IllegalArgumentException("JSON: " + what + " at offset " + at);
    }
}
