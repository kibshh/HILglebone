package sessions

import (
	"encoding/json"
	"errors"
	"net/http"
	"strconv"

	"github.com/google/uuid"

	"github.com/kibshh/HILglebone/backend/internal/httpx"
)

const maxBodyBytes = 64 * 1024

type Handler struct {
	service *Service
}

func NewHandler(service *Service) *Handler {
	return &Handler{service: service}
}

func (h *Handler) Allocate(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, maxBodyBytes)
	var req AllocateRequest
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		httpx.WriteJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid json"})
		return
	}
	if req.UserID == uuid.Nil || req.BBBDeviceID == uuid.Nil || req.DUTDeviceID == uuid.Nil {
		httpx.WriteJSON(w, http.StatusBadRequest,
			map[string]string{"error": "user_id, bbb_device_id, and dut_device_id are required"})
		return
	}

	session, err := h.service.Allocate(r.Context(), req)
	if err != nil {
		writeServiceError(w, err)
		return
	}
	httpx.WriteJSON(w, http.StatusCreated, session)
}

func (h *Handler) Start(w http.ResponseWriter, r *http.Request) {
	id, ok := parsePathID(w, r)
	if !ok {
		return
	}
	session, err := h.service.Start(r.Context(), id)
	if err != nil {
		writeServiceError(w, err)
		return
	}
	httpx.WriteJSON(w, http.StatusOK, session)
}

func (h *Handler) Stop(w http.ResponseWriter, r *http.Request) {
	id, ok := parsePathID(w, r)
	if !ok {
		return
	}
	session, err := h.service.Stop(r.Context(), id)
	if err != nil {
		writeServiceError(w, err)
		return
	}
	httpx.WriteJSON(w, http.StatusOK, session)
}

func (h *Handler) Get(w http.ResponseWriter, r *http.Request) {
	id, ok := parsePathID(w, r)
	if !ok {
		return
	}
	session, err := h.service.Get(r.Context(), id)
	if err != nil {
		writeServiceError(w, err)
		return
	}
	httpx.WriteJSON(w, http.StatusOK, session)
}

func (h *Handler) List(w http.ResponseWriter, r *http.Request) {
	var filter ListFilter
	if userIDStr := r.URL.Query().Get("user_id"); userIDStr != "" {
		parsed, err := uuid.Parse(userIDStr)
		if err != nil {
			httpx.WriteJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid user_id"})
			return
		}
		filter.UserID = &parsed
	}
	filter.Status = r.URL.Query().Get("status")
	if limitStr := r.URL.Query().Get("limit"); limitStr != "" {
		n, err := strconv.Atoi(limitStr)
		if err != nil || n < 1 {
			httpx.WriteJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid limit"})
			return
		}
		filter.Limit = &n
	}

	result, err := h.service.List(r.Context(), filter)
	if err != nil {
		writeServiceError(w, err)
		return
	}
	if result == nil {
		result = []*Session{}
	}
	httpx.WriteJSON(w, http.StatusOK, result)
}

func parsePathID(w http.ResponseWriter, r *http.Request) (uuid.UUID, bool) {
	id, err := uuid.Parse(r.PathValue("id"))
	if err != nil {
		httpx.WriteJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid session id"})
		return uuid.Nil, false
	}
	return id, true
}

func writeServiceError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, ErrNotFound):
		httpx.WriteJSON(w, http.StatusNotFound, map[string]string{"error": "not found"})
	case errors.Is(err, ErrInvalidTransition):
		httpx.WriteJSON(w, http.StatusConflict, map[string]string{"error": "invalid state for transition"})
	case errors.Is(err, ErrBBBBusy):
		httpx.WriteJSON(w, http.StatusConflict, map[string]string{"error": "bbb already has an active session"})
	case errors.Is(err, ErrInvalidReference):
		httpx.WriteJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid reference"})
	default:
		httpx.WriteJSON(w, http.StatusInternalServerError, map[string]string{"error": "internal"})
	}
}
