package firmware

import (
	"errors"
	"net/http"

	"github.com/google/uuid"

	"github.com/kibshh/HILglebone/backend/internal/httpx"
)

const (
	MB = 1 << 20

	// Cap firmware blobs at 64 MB. STM32-class firmware is typically <512 KB
	// but the platform also targets larger MCUs (e.g. nRF52840, ESP32) whose
	// images can be a few MB; 64 MB leaves headroom while preventing a hostile
	// client from streaming gigabytes through the backend.
	maxFirmwareBytes = 64 * MB

	// Bytes the multipart parser may hold in memory before spilling to disk.
	// Set just below maxFirmwareBytes so most uploads stay in-memory.
	multipartMemory = 32 * MB
)

type Handler struct {
	service *Service
}

func NewHandler(service *Service) *Handler {
	return &Handler{service: service}
}

func (h *Handler) Upload(w http.ResponseWriter, r *http.Request) {
	// Extra slop above maxFirmwareBytes covers multipart envelope overhead.
	r.Body = http.MaxBytesReader(w, r.Body, maxFirmwareBytes+MB)

	if err := r.ParseMultipartForm(multipartMemory); err != nil {
		var maxErr *http.MaxBytesError
		if errors.As(err, &maxErr) {
			httpx.WriteJSON(w, http.StatusRequestEntityTooLarge,
				map[string]string{"error": "firmware too large"})
			return
		}
		httpx.WriteJSON(w, http.StatusBadRequest,
			map[string]string{"error": "invalid multipart form"})
		return
	}

	userID, err := uuid.Parse(r.FormValue("user_id"))
	if err != nil {
		httpx.WriteJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid user_id"})
		return
	}
	dutDeviceID, err := uuid.Parse(r.FormValue("dut_device_id"))
	if err != nil {
		httpx.WriteJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid dut_device_id"})
		return
	}

	file, header, err := r.FormFile("file")
	if err != nil {
		httpx.WriteJSON(w, http.StatusBadRequest, map[string]string{"error": "missing file"})
		return
	}
	defer file.Close()

	if header.Size <= 0 {
		httpx.WriteJSON(w, http.StatusBadRequest, map[string]string{"error": "empty file"})
		return
	}
	if header.Size > maxFirmwareBytes {
		httpx.WriteJSON(w, http.StatusRequestEntityTooLarge,
			map[string]string{"error": "firmware too large"})
		return
	}

	result, err := h.service.Upload(r.Context(), UploadRequest{
		UserID:      userID,
		DUTDeviceID: dutDeviceID,
		Filename:    header.Filename,
		Data:        file,
		Size:        header.Size,
	})
	if err != nil {
		if errors.Is(err, ErrInvalidReference) {
			httpx.WriteJSON(w, http.StatusBadRequest,
				map[string]string{"error": "invalid user_id or dut_device_id"})
			return
		}
		if errors.Is(err, ErrEmpty) {
			httpx.WriteJSON(w, http.StatusBadRequest, map[string]string{"error": "empty file"})
			return
		}
		httpx.WriteJSON(w, http.StatusInternalServerError, map[string]string{"error": "internal"})
		return
	}

	httpx.WriteJSON(w, http.StatusCreated, result)
}
