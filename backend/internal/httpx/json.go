// Package httpx contains small HTTP helpers shared across handlers.
package httpx

import (
	"bytes"
	"encoding/json"
	"log/slog"
	"net/http"
)

// WriteJSON serializes body to JSON and writes it to w with the given status.
//
// Encoding is done into a buffer first so a marshal error produces a clean 500
// instead of a half-written response. A failure to flush bytes to the client
// (broken connection, client gone) is only logged — at that point the status
// line has already been sent and nothing else can be returned.
//
// Internal failures here are catastrophic-rare (a programmer passed a
// non-encodable value, or the socket died mid-response) so we log via the
// package-level slog default rather than threading a logger through every call.
func WriteJSON(w http.ResponseWriter, status int, body any) {
	var buf bytes.Buffer
	if err := json.NewEncoder(&buf).Encode(body); err != nil {
		slog.Error("json encode failed", "error", err)
		http.Error(w, "internal", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if _, err := w.Write(buf.Bytes()); err != nil {
		slog.Error("response write failed", "error", err)
	}
}
