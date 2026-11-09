// SPDX-License-Identifier: MIT
package org.keel.demo;

import java.math.BigDecimal;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Small bounded JSON codec; no Java object deserialization or runtime dependencies. */
final class Json {
    private final String source;
    private int at;
    private Json(String source) { this.source = source; }
    static Map<String,Object> object(String source) {
        Json p = new Json(source);
        Object v = p.value(0);
        p.space();
        if (p.at != source.length() || !(v instanceof Map<?,?>)) throw bad();
        @SuppressWarnings("unchecked") Map<String,Object> map = (Map<String,Object>)v;
        return map;
    }
    private static IllegalArgumentException bad() { return new IllegalArgumentException("Invalid JSON"); }
    private void space() { while (at<source.length() && " \t\r\n".indexOf(source.charAt(at))>=0) at++; }
    private boolean take(char c) { space(); if(at<source.length() && source.charAt(at)==c) { at++; return true; } return false; }
    private Object value(int depth) {
        if(depth>16) throw bad();
        space(); if(at>=source.length()) throw bad();
        char c=source.charAt(at);
        if(c=='"') return string();
        if(take('{')) {
            Map<String,Object> m=new LinkedHashMap<>();
            if(take('}')) return m;
            do { space(); if(at>=source.length() || source.charAt(at)!='"') throw bad();
                String key=string(); if(!take(':') || m.containsKey(key)) throw bad();
                m.put(key,value(depth+1)); if(m.size()>64) throw bad();
            } while(take(','));
            if(!take('}')) throw bad(); return m;
        }
        if(take('[')) {
            List<Object> l=new ArrayList<>(); if(take(']')) return l;
            do { l.add(value(depth+1)); if(l.size()>128) throw bad(); } while(take(','));
            if(!take(']')) throw bad(); return l;
        }
        for(String literal:List.of("true","false","null")) {
            if(source.startsWith(literal,at)) { at+=literal.length(); return literal.equals("null")?null:literal.equals("true"); }
        }
        int start=at;
        if(c=='-') at++;
        if(at>=source.length()) throw bad();
        if(source.charAt(at)=='0') at++;
        else { if(source.charAt(at)<'1'||source.charAt(at)>'9') throw bad(); while(at<source.length() && Character.isDigit(source.charAt(at))) at++; }
        if(at<source.length() && source.charAt(at)=='.') { at++; int a=at; while(at<source.length()&&Character.isDigit(source.charAt(at))) at++; if(a==at) throw bad(); }
        if(at<source.length() && "eE".indexOf(source.charAt(at))>=0) { at++; if(at<source.length()&&"+-".indexOf(source.charAt(at))>=0) at++; int a=at; while(at<source.length()&&Character.isDigit(source.charAt(at))) at++; if(a==at) throw bad(); }
        try { return new BigDecimal(source.substring(start,at)); } catch(NumberFormatException e) { throw bad(); }
    }
    private String string() {
        at++; StringBuilder b=new StringBuilder();
        while(at<source.length()) {
            char c=source.charAt(at++); if(c=='"') return b.toString(); if(c<32) throw bad();
            if(c=='\\') {
                if(at>=source.length()) throw bad(); char e=source.charAt(at++);
                switch(e) {
                    case '"','\\','/' -> b.append(e);
                    case 'b' -> b.append('\b'); case 'f' -> b.append('\f');
                    case 'n' -> b.append('\n'); case 'r' -> b.append('\r'); case 't' -> b.append('\t');
                    case 'u' -> { if(at+4>source.length()) throw bad(); try { b.append((char)Integer.parseInt(source.substring(at,at+4),16)); } catch(NumberFormatException ex) { throw bad(); } at+=4; }
                    default -> throw bad();
                }
            } else b.append(c);
        } throw bad();
    }
    static Map<String,Object> map(Object... pairs) {
        Map<String,Object> m=new LinkedHashMap<>();
        for(int i=0;i<pairs.length;i+=2) m.put((String)pairs[i],pairs[i+1]); return m;
    }
    static String text(Map<String,Object> map,String key) {
        Object v=map.get(key); if(!(v instanceof String s)||s.length()>128) throw new IllegalArgumentException(key+" must be a short string"); return s;
    }
    static long integer(Map<String,Object> map,String key,long lo,long hi) {
        Object v=map.get(key); long n;
        try { if(v instanceof BigDecimal b) n=b.longValueExact(); else if(v instanceof Integer||v instanceof Long) n=((Number)v).longValue(); else throw bad(); }
        catch(ArithmeticException e) { throw new IllegalArgumentException(key+" must be an integer"); }
        if(n<lo||n>hi) throw new IllegalArgumentException(key+" must be "+lo+".."+hi); return n;
    }
    static String encode(Object v) {
        if(v==null) return "null";
        if(v instanceof String s) {
            StringBuilder b=new StringBuilder("\""); for(char c:s.toCharArray()) switch(c) {
                case '"' -> b.append("\\\""); case '\\' -> b.append("\\\\");
                case '\n' -> b.append("\\n"); case '\r' -> b.append("\\r"); case '\t' -> b.append("\\t");
                default -> { if(c<32) b.append(String.format("\\u%04x",(int)c)); else b.append(c); }
            } return b.append('"').toString();
        }
        if(v instanceof Number n) { if(!Double.isFinite(n.doubleValue())) return "null"; return n.toString(); }
        if(v instanceof Boolean) return v.toString();
        if(v instanceof Map<?,?> m) { List<String> parts=new ArrayList<>(); m.forEach((k,value)->parts.add(encode(k.toString())+":"+encode(value))); return "{"+String.join(",",parts)+"}"; }
        if(v instanceof Iterable<?> l) { List<String> parts=new ArrayList<>(); l.forEach(value->parts.add(encode(value))); return "["+String.join(",",parts)+"]"; }
        if(v instanceof int[] a) { List<Integer> l=new ArrayList<>(); for(int n:a) l.add(n); return encode(l); }
        throw new IllegalArgumentException("Unsupported JSON type: "+v.getClass());
    }
}
