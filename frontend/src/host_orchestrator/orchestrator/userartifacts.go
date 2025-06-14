// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package orchestrator

import (
	"archive/zip"
	"crypto/sha256"
	"fmt"
	"io"
	"io/ioutil"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"

	apiv1 "github.com/google/android-cuttlefish/frontend/src/host_orchestrator/api/v1"
	hoexec "github.com/google/android-cuttlefish/frontend/src/host_orchestrator/orchestrator/exec"
	"github.com/google/android-cuttlefish/frontend/src/liboperator/operator"
	"github.com/google/btree"
	"github.com/google/uuid"
)

// Resolves the user artifacts full directory.
type UserArtifactsDirResolver interface {
	// Given a directory name returns its full path.
	GetDirPath(name string) string
}

type UserArtifactChunk struct {
	Name          string
	File          io.Reader
	OffsetBytes   int64
	SizeBytes     int64
	FileSizeBytes int64
}

// Abstraction for managing user artifacts for launching CVDs.
type UserArtifactsManager interface {
	// Update artifact with the passed chunk
	UpdateArtifact(checksum string, chunk UserArtifactChunk) error
	// Stat artifact whether artifact with given checksum exists or not.
	StatArtifact(checksum string) (*apiv1.StatArtifactResponse, error)

	UserArtifactsDirResolver
	// Creates a new directory for uploading user artifacts in the future.
	// Deprecated: use `UpdateArtifact` instead
	NewDir() (*apiv1.UploadDirectory, error)
	// List existing directories
	// Deprecated: use `UpdateArtifact` instead
	ListDirs() (*apiv1.ListUploadDirectoriesResponse, error)
	// Update artifact with the passed chunk
	// Deprecated: use `UpdateArtifact` instead
	UpdateArtifactWithDir(dir string, chunk UserArtifactChunk) error
	// Extract artifact
	ExtractArtifactWithDir(dir, name string) error
}

// Options for creating instances of UserArtifactsManager implementations.
type UserArtifactsManagerOpts struct {
	// Root directory for legacy APIs treating user artifacts.
	LegacyRootDir string
	// Root directory for storing user artifacts. After Host Orchestrator moves user artifacts from
	// the working directory into here, it becomes immutable unless introducing any replacement
	// algorithm(e.g. LRU) on RootDir to manage the storage.
	RootDir string
}

// An implementation of the UserArtifactsManager interface.
type UserArtifactsManagerImpl struct {
	UserArtifactsManagerOpts
	// The root directory where to store artifacts with legacy API calls.
	commonWorkDir string
	UuidWorkDir   string
	mutexes       sync.Map
	chunkStates   sync.Map
}

// Creates a new instance of UserArtifactsManagerImpl.
func NewUserArtifactsManagerImpl(opts UserArtifactsManagerOpts) (*UserArtifactsManagerImpl, error) {
	commonWorkDir := filepath.Join(opts.RootDir, "working")
	UuidWorkDir := filepath.Join(commonWorkDir, uuid.New().String())
	if err := os.RemoveAll(UuidWorkDir); err != nil {
		return nil, err
	}
	return &UserArtifactsManagerImpl{
		UserArtifactsManagerOpts: opts,
		commonWorkDir:            commonWorkDir,
		UuidWorkDir:              UuidWorkDir,
	}, nil
}

func (m *UserArtifactsManagerImpl) NewDir() (*apiv1.UploadDirectory, error) {
	if err := createDir(m.LegacyRootDir); err != nil {
		return nil, err
	}
	dir, err := createNewUADir(m.LegacyRootDir)
	if err != nil {
		return nil, err
	}
	log.Println("created new user artifact directory", dir)
	return &apiv1.UploadDirectory{Name: filepath.Base(dir)}, nil
}

func (m *UserArtifactsManagerImpl) ListDirs() (*apiv1.ListUploadDirectoriesResponse, error) {
	exist, err := fileExist(m.LegacyRootDir)
	if err != nil {
		return nil, err
	}
	if !exist {
		return &apiv1.ListUploadDirectoriesResponse{Items: make([]*apiv1.UploadDirectory, 0)}, nil
	}
	entries, err := ioutil.ReadDir(m.LegacyRootDir)
	if err != nil {
		return nil, err
	}
	dirs := make([]*apiv1.UploadDirectory, 0)
	for _, entry := range entries {
		if entry.IsDir() {
			dirs = append(dirs, &apiv1.UploadDirectory{Name: entry.Name()})
		}
	}
	return &apiv1.ListUploadDirectoriesResponse{Items: dirs}, nil
}

func (m *UserArtifactsManagerImpl) GetDirPath(name string) string {
	return filepath.Join(m.LegacyRootDir, name)
}

func (m *UserArtifactsManagerImpl) GetFilePath(dir, filename string) string {
	return filepath.Join(m.LegacyRootDir, dir, filename)
}

func (m *UserArtifactsManagerImpl) UpdateArtifact(checksum string, chunk UserArtifactChunk) error {
	if chunk.OffsetBytes < 0 {
		return operator.NewBadRequestError(fmt.Sprintf("invalid value (chunk_offset:%d)", chunk.OffsetBytes), nil)
	}
	if chunk.SizeBytes < 0 {
		return fmt.Errorf("invalid value (chunk_size:%d)", chunk.SizeBytes)
	}
	if chunk.FileSizeBytes < 0 {
		return operator.NewBadRequestError(fmt.Sprintf("invalid value (file_size:%d)", chunk.FileSizeBytes), nil)
	}
	if chunk.OffsetBytes+chunk.SizeBytes > chunk.FileSizeBytes {
		return operator.NewBadRequestError(fmt.Sprintf("invalid value pair (chunk_offset:%d, chunk_size:%d, file_size:%d)", chunk.OffsetBytes, chunk.SizeBytes, chunk.FileSizeBytes), nil)
	}
	mayMoveArtifact, err := m.writeChunkAndUpdateState(checksum, chunk)
	if err != nil {
		return fmt.Errorf("failed to update chunk: %w", err)
	}
	if mayMoveArtifact {
		if err := m.moveArtifactIfNeeded(checksum, chunk); err != nil {
			return fmt.Errorf("failed to move the user artifact from working directory: %w", err)
		}
	}
	return nil
}

func (m *UserArtifactsManagerImpl) UpdateArtifactWithDir(dir string, chunk UserArtifactChunk) error {
	dir = filepath.Join(m.LegacyRootDir, dir)
	if ok, err := fileExist(dir); err != nil {
		return err
	} else if !ok {
		return operator.NewBadRequestError("upload directory %q does not exist", err)
	}
	filename := filepath.Join(dir, chunk.Name)
	if err := createUAFile(filename); err != nil {
		return err
	}
	if err := writeChunk(filename, chunk); err != nil {
		return err
	}
	return nil
}

func (m *UserArtifactsManagerImpl) StatArtifact(checksum string) (*apiv1.StatArtifactResponse, error) {
	if exists, err := dirExists(filepath.Join(m.RootDir, checksum)); err != nil {
		return nil, fmt.Errorf("failed to check existence of directory: %w", err)
	} else if !exists {
		return nil, operator.NewNotFoundError(fmt.Sprintf("user artifact(checksum:%q) not found", checksum), nil)
	}
	return &apiv1.StatArtifactResponse{}, nil
}

func (m *UserArtifactsManagerImpl) getRWMutex(checksum string) *sync.RWMutex {
	mu, _ := m.mutexes.LoadOrStore(checksum, &sync.RWMutex{})
	return mu.(*sync.RWMutex)
}

func (m *UserArtifactsManagerImpl) getChunkState(checksum string, fileSize int64) *chunkState {
	cs, _ := m.chunkStates.LoadOrStore(checksum, NewChunkState(fileSize))
	return cs.(*chunkState)
}

func dirExists(dir string) (bool, error) {
	if info, err := os.Stat(dir); os.IsNotExist(err) {
		return false, nil
	} else if err != nil {
		return false, err
	} else {
		return info.IsDir(), nil
	}
}

func writeChunk(filename string, chunk UserArtifactChunk) error {
	f, err := os.OpenFile(filename, os.O_WRONLY|os.O_CREATE, 0664)
	if err != nil {
		return fmt.Errorf("failed to open or create working file: %w", err)
	}
	defer f.Close()
	if _, err := f.Seek(chunk.OffsetBytes, io.SeekStart); err != nil {
		return fmt.Errorf("failed to seek offset of working file: %w", err)
	}
	if _, err := io.Copy(f, chunk.File); err != nil {
		return fmt.Errorf("failed to copy chunk into working file: %w", err)
	}
	return nil
}

// Calculating the checksum of the user artifact is a heavy task. Instead, it records the state
// of updated chunks to know whether it's ready to calculate checksum and move the user artifact or
// not. This function returns the boolean value as true if the user artifact is fully updated so it
// needs to calculate checksum later.
func (m *UserArtifactsManagerImpl) writeChunkAndUpdateState(checksum string, chunk UserArtifactChunk) (bool, error) {
	mu := m.getRWMutex(checksum)
	// Reason for acquiring read lock is to allow updating multiple chunks concurrently, but to
	// restrict validating checksum or moving artifact into different directory when one or more
	// chunks are being updated.
	mu.RLock()
	defer mu.RUnlock()
	if exists, err := dirExists(filepath.Join(m.RootDir, checksum)); err != nil {
		return false, fmt.Errorf("failed to check existence of directory: %w", err)
	} else if exists {
		return false, operator.NewConflictError(fmt.Sprintf("user artifact(checksum:%q) already exists", checksum), nil)
	}
	if err := createDir(m.RootDir); err != nil {
		return false, fmt.Errorf("failed to create root directory for storing user artifacts: %w", err)
	}
	if err := createDir(m.commonWorkDir); err != nil {
		return false, fmt.Errorf("failed to create working directory for all Host Orchestrators: %w", err)
	}
	if err := createDir(m.UuidWorkDir); err != nil {
		return false, fmt.Errorf("failed to create working directory for this Host Orchestrator: %w", err)
	}
	workDir := filepath.Join(m.UuidWorkDir, checksum)
	if err := createDir(workDir); err != nil {
		return false, fmt.Errorf("failed to create working directory: %w", err)
	}
	workFile := filepath.Join(workDir, chunk.Name)
	if err := writeChunk(workFile, chunk); err != nil {
		return false, err
	}
	cs := m.getChunkState(checksum, chunk.FileSizeBytes)
	cs.Update(chunk.OffsetBytes, chunk.OffsetBytes+chunk.SizeBytes)
	return cs.IsCompleted(), nil
}

func (m *UserArtifactsManagerImpl) validateChecksum(checksum string, chunk UserArtifactChunk) (bool, error) {
	workFile := filepath.Join(m.UuidWorkDir, checksum, chunk.Name)
	f, err := os.Open(workFile)
	if err != nil {
		return false, fmt.Errorf("failed to open working file: %w", err)
	}
	defer f.Close()
	if info, err := f.Stat(); err != nil {
		return false, err
	} else if chunk.FileSizeBytes != info.Size() {
		return false, nil
	}
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return false, err
	}
	return checksum == fmt.Sprintf("%x", h.Sum(nil)), nil
}

func (m *UserArtifactsManagerImpl) moveArtifactIfNeeded(checksum string, chunk UserArtifactChunk) error {
	mu := m.getRWMutex(checksum)
	// Reason for acquiring write lock is to restrict writing new chunks while validating checksum
	// or moving artifact.
	mu.Lock()
	defer mu.Unlock()
	dir := filepath.Join(m.RootDir, checksum)
	if exists, err := dirExists(dir); err != nil {
		return fmt.Errorf("failed to check existence of directory: %w", err)
	} else if exists {
		return nil
	}
	if matches, err := m.validateChecksum(checksum, chunk); err != nil {
		return fmt.Errorf("failed to validate checksum of the user artifact: %w", err)
	} else if !matches {
		// Only if the checksum matches, it's ready to move the user artifact from working
		// directory.
		return nil
	}
	workDir := filepath.Join(m.UuidWorkDir, checksum)
	if err := os.Rename(workDir, dir); err != nil {
		return fmt.Errorf("failed to move the user artifact: %w", err)
	}
	m.chunkStates.Delete(checksum)
	return nil
}

func (m *UserArtifactsManagerImpl) ExtractArtifactWithDir(dir, name string) error {
	dir = filepath.Join(m.LegacyRootDir, dir)
	if ok, err := fileExist(dir); err != nil {
		return err
	} else if !ok {
		return operator.NewBadRequestError(fmt.Sprintf("directory %q does not exist", dir), nil)
	}
	filename := filepath.Join(dir, name)
	if ok, err := fileExist(filename); err != nil {
		return err
	} else if !ok {
		return operator.NewBadRequestError(fmt.Sprintf("artifact %q does not exist", name), nil)
	}
	if strings.HasSuffix(filename, ".tar.gz") {
		if err := Untar(dir, filename); err != nil {
			return fmt.Errorf("failed extracting %q: %w", name, err)
		}
	} else if strings.HasSuffix(filename, ".zip") {
		if err := Unzip(dir, filename); err != nil {
			return fmt.Errorf("failed extracting %q: %w", name, err)
		}
	} else {
		return operator.NewBadRequestError(fmt.Sprintf("unsupported extension: %q", name), nil)
	}
	return nil
}

func Untar(dst string, src string) error {
	_, err := hoexec.Exec(exec.CommandContext, "tar", "-xf", src, "-C", dst)
	if err != nil {
		return err
	}
	return nil
}

func Unzip(dstDir string, src string) error {
	r, err := zip.OpenReader(src)
	if err != nil {
		return err
	}
	defer r.Close()
	extractTo := func(dst string, src *zip.File) error {
		rc, err := src.Open()
		if err != nil {
			return err
		}
		defer rc.Close()
		if err := createUAFile(dst); err != nil {
			return err
		}
		dstFile, err := os.OpenFile(dst, os.O_WRONLY, 0664)
		if err != nil {
			return err
		}
		defer dstFile.Close()
		if _, err := io.Copy(dstFile, rc); err != nil {
			return err
		}
		return nil
	}
	for _, f := range r.File {
		// Do not extract nested dirs as ci.android.com img.zip artifact
		// does not contain nested dirs.
		if f.Mode().IsDir() {
			continue
		}
		if err := extractTo(filepath.Join(dstDir, f.Name), f); err != nil {
			return err
		}
	}
	return nil
}

func createNewUADir(parent string) (string, error) {
	ctx := exec.CommandContext
	stdout, err := hoexec.Exec(ctx, "mktemp", "--directory", "-p", parent)
	if err != nil {
		return "", err
	}
	name := strings.TrimRight(stdout, "\n")
	// Sets permission regardless of umask.
	if _, err := hoexec.Exec(ctx, "chmod", "u=rwx,g=rwx,o=r", name); err != nil {
		return "", err
	}
	return name, nil
}

func createUAFile(filename string) error {
	ctx := exec.CommandContext
	_, err := hoexec.Exec(ctx, "touch", filename)
	if err != nil {
		return err
	}
	// Sets permission regardless of umask.
	if _, err := hoexec.Exec(ctx, "chmod", "u=rwx,g=rw,o=r", filename); err != nil {
		return err
	}
	return nil
}

// Structure for managing the state of updated chunk for efficiently knowing whether the user
// artifact may need to calculate hash sum or not.
type chunkState struct {
	fileSize int64
	// Items are stored with a sequence of alternating true/false isUpdated field and increasing
	// offset field.
	items *btree.BTree
	mutex sync.RWMutex
}

type chunkStateItem struct {
	// Starting byte offset of the continuous range having same state whether updated or not.
	offset int64
	// State description whether given byte offset of the user artifact is updated or not.
	isUpdated bool
}

func (i chunkStateItem) Less(item btree.Item) bool {
	return i.offset < item.(chunkStateItem).offset
}

func NewChunkState(fileSize int64) *chunkState {
	cs := chunkState{
		fileSize: fileSize,
		items:    btree.New(2),
		mutex:    sync.RWMutex{},
	}
	return &cs
}

func (cs *chunkState) getItemOrPrev(offset int64) *chunkStateItem {
	var record *chunkStateItem
	cs.items.DescendLessOrEqual(chunkStateItem{offset: offset}, func(item btree.Item) bool {
		entry := item.(chunkStateItem)
		record = &entry
		return false
	})
	return record
}

func (cs *chunkState) getItemOrNext(offset int64) *chunkStateItem {
	var record *chunkStateItem
	cs.items.AscendGreaterOrEqual(chunkStateItem{offset: offset}, func(item btree.Item) bool {
		entry := item.(chunkStateItem)
		record = &entry
		return false
	})
	return record
}

func (cs *chunkState) Update(start int64, end int64) error {
	if start < 0 {
		return fmt.Errorf("invalid start offset of the range")
	}
	if end > cs.fileSize {
		return fmt.Errorf("invalid end offset of the range")
	}
	if start >= end {
		return fmt.Errorf("start offset should be less than end offset")
	}
	cs.mutex.Lock()
	defer cs.mutex.Unlock()

	// Remove all current state between the start offset and the end offset.
	for item := cs.getItemOrNext(start); item != nil && item.offset <= end; item = cs.getItemOrNext(start) {
		cs.items.Delete(*item)
	}
	// State of the start offset is updated according to the state of the previous item.
	if prev := cs.getItemOrPrev(start); prev == nil || !prev.isUpdated {
		cs.items.ReplaceOrInsert(chunkStateItem{offset: start, isUpdated: true})
	}
	// State of the end offset is updated according to the state of the next item.
	if next := cs.getItemOrNext(end); next == nil || next.isUpdated {
		cs.items.ReplaceOrInsert(chunkStateItem{offset: end, isUpdated: false})
	}
	return nil
}

func (cs *chunkState) IsCompleted() bool {
	cs.mutex.RLock()
	defer cs.mutex.RUnlock()
	if cs.items.Len() != 2 {
		return false
	}
	first := chunkStateItem{offset: 0, isUpdated: true}
	last := chunkStateItem{offset: cs.fileSize, isUpdated: false}
	return cs.items.Min() == first && cs.items.Max() == last
}
