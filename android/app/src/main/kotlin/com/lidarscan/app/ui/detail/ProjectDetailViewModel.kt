package com.lidarscan.app.ui.detail

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.core.store.Project
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

sealed interface ProjectDetailUiState {
    data object Loading : ProjectDetailUiState
    data class Loaded(val project: Project) : ProjectDetailUiState
    data object NotFound : ProjectDetailUiState
}

class ProjectDetailViewModel(
    private val projectStore: ProjectStore,
    private val projectId: String,
) : ViewModel() {

    private val _uiState = MutableStateFlow<ProjectDetailUiState>(ProjectDetailUiState.Loading)
    val uiState: StateFlow<ProjectDetailUiState> = _uiState.asStateFlow()

    init {
        load()
    }

    fun load() {
        viewModelScope.launch(Dispatchers.IO) {
            val project = projectStore.open(projectId)
            _uiState.value = if (project != null) {
                ProjectDetailUiState.Loaded(project)
            } else {
                ProjectDetailUiState.NotFound
            }
        }
    }
}
