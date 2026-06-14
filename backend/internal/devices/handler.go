package devices

import (
	"encoding/json"
	"errors"
	"net/http"

	"github.com/kibshh/HILglebone/backend/internal/httpx"
)

// A real register body is a few hundred bytes (name + token + small JSON capabilities).
// 64 KB is comfortably above any plausible payload but small enough that a hostile
// client cannot tie up server memory by streaming a giant body before being rejected.
const maxRegisterBodyBytes = 64 * 1024

type Handler struct {
	service *Service
}

func NewHandler(service *Service) *Handler {
	return &Handler{service: service}
}

func (h *Handler) Register(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, maxRegisterBodyBytes)

	var req RegisterRequest
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		httpx.WriteJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid json"})
		return
	}

	result, err := h.service.Register(r.Context(), req)
	if err != nil {
		if errors.Is(err, ErrUnauthorized) {
			httpx.WriteJSON(w, http.StatusUnauthorized, map[string]string{"error": "unauthorized"})
			return
		}
		httpx.WriteJSON(w, http.StatusInternalServerError, map[string]string{"error": "internal"})
		return
	}

	httpx.WriteJSON(w, http.StatusOK, result)
}
