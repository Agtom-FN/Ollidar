package com.lidarscan.app.ui.projects

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.core.store.Project
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class ProjectsUiState(
    val loading: Boolean = true,
    val projects: List<Project> = emptyList(),
)

class ProjectsListViewModel(private val projectStore: ProjectStore) : ViewModel() {

    private val _uiState = MutableStateFlow(ProjectsUiState())
    val uiState: StateFlow<ProjectsUiState> = _uiState.asStateFlow()

    init {
        refresh()
    }

    fun refresh() {
        viewModelScope.launch(Dispatchers.IO) {
            val projects = projectStore.list()
            _uiState.update { it.copy(loading = false, projects = projects) }
        }
    }

    fun delete(projectId: String) {
        viewModelScope.launch(Dispatchers.IO) {
            projectStore.delete(projectId)
            refresh()
        }
    }
}
