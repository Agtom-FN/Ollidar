package com.lidarscan.app.ui.newproject

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile
import com.lidarscan.core.store.ProjectStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

class NewProjectViewModel(private val projectStore: ProjectStore) : ViewModel() {

    var name by mutableStateOf("")
        private set

    var sensor by mutableStateOf(SensorType.MID360)
        private set

    var profile by mutableStateOf(WorkflowProfile.QUICK_SCAN)
        private set

    var isCreating by mutableStateOf(false)
        private set

    private val _createdProjectId = MutableStateFlow<String?>(null)
    val createdProjectId: StateFlow<String?> = _createdProjectId.asStateFlow()

    val canCreate: Boolean get() = name.isNotBlank() && !isCreating

    fun onNameChange(value: String) {
        name = value
    }

    fun onSensorChange(value: SensorType) {
        sensor = value
    }

    fun onProfileChange(value: WorkflowProfile) {
        profile = value
    }

    fun create() {
        if (!canCreate) return
        isCreating = true
        viewModelScope.launch(Dispatchers.IO) {
            val project = projectStore.create(name.trim(), sensor, profile)
            _createdProjectId.value = project.id
        }
    }
}
