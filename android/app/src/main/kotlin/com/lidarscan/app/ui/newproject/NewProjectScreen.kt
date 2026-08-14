package com.lidarscan.app.ui.newproject

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.selection.selectable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.lidarscan.app.di.AppContainer
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile

@Composable
fun NewProjectRoute(
    container: AppContainer,
    onCreated: (String) -> Unit,
    onCancel: () -> Unit,
) {
    val viewModel: NewProjectViewModel = viewModel(
        factory = viewModelFactory {
            initializer { NewProjectViewModel(container.projectStore) }
        },
    )
    val createdProjectId by viewModel.createdProjectId.collectAsStateWithLifecycle()

    LaunchedEffect(createdProjectId) {
        createdProjectId?.let(onCreated)
    }

    NewProjectScreen(
        name = viewModel.name,
        sensor = viewModel.sensor,
        profile = viewModel.profile,
        canCreate = viewModel.canCreate,
        isCreating = viewModel.isCreating,
        onNameChange = viewModel::onNameChange,
        onSensorChange = viewModel::onSensorChange,
        onProfileChange = viewModel::onProfileChange,
        onCreate = viewModel::create,
        onCancel = onCancel,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun NewProjectScreen(
    name: String,
    sensor: SensorType,
    profile: WorkflowProfile,
    canCreate: Boolean,
    isCreating: Boolean,
    onNameChange: (String) -> Unit,
    onSensorChange: (SensorType) -> Unit,
    onProfileChange: (WorkflowProfile) -> Unit,
    onCreate: () -> Unit,
    onCancel: () -> Unit,
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("New project") },
                navigationIcon = {
                    IconButton(onClick = onCancel) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Cancel")
                    }
                },
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
        ) {
            OutlinedTextField(
                value = name,
                onValueChange = onNameChange,
                label = { Text("Project name") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )

            Spacer(Modifier.height(24.dp))
            Text("Sensor", style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(8.dp))
            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                SensorType.entries.forEachIndexed { index, option ->
                    SegmentedButton(
                        selected = sensor == option,
                        onClick = { onSensorChange(option) },
                        shape = SegmentedButtonDefaults.itemShape(index = index, count = SensorType.entries.size),
                    ) {
                        Text(option.displayName)
                    }
                }
            }

            Spacer(Modifier.height(24.dp))
            Text("Workflow profile", style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(8.dp))
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                WorkflowProfile.entries.forEach { option ->
                    ProfileOptionCard(
                        profile = option,
                        selected = profile == option,
                        onClick = { onProfileChange(option) },
                    )
                }
            }

            Spacer(Modifier.height(28.dp))
            Button(
                onClick = onCreate,
                enabled = canCreate,
                modifier = Modifier.fillMaxWidth(),
            ) {
                if (isCreating) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(20.dp),
                        color = MaterialTheme.colorScheme.onPrimary,
                        strokeWidth = 2.dp,
                    )
                } else {
                    Text("Create project")
                }
            }
        }
    }
}

@Composable
private fun ProfileOptionCard(
    profile: WorkflowProfile,
    selected: Boolean,
    onClick: () -> Unit,
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .selectable(selected = selected, onClick = onClick),
        colors = if (selected) {
            CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer)
        } else {
            CardDefaults.cardColors()
        },
    ) {
        Row(
            modifier = Modifier.padding(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            RadioButton(selected = selected, onClick = onClick)
            Spacer(Modifier.width(8.dp))
            Column {
                Text(profile.displayName, style = MaterialTheme.typography.titleSmall)
                Text(
                    text = profile.description,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}
