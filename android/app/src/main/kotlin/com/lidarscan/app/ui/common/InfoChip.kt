package com.lidarscan.app.ui.common

import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.lidarscan.app.ui.theme.SensorD6Badge
import com.lidarscan.app.ui.theme.SensorMid360Badge
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.model.WorkflowProfile

/** Small pill used for read-only metadata (sensor badge, profile chip). Not clickable, on purpose — these describe the project, they don't act on it. */
@Composable
fun InfoChip(
    text: String,
    color: Color,
    modifier: Modifier = Modifier,
) {
    Surface(
        modifier = modifier,
        color = color.copy(alpha = 0.16f),
        contentColor = color,
        shape = RoundedCornerShape(50),
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.labelMedium,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
        )
    }
}

@Composable
fun SensorBadge(sensor: SensorType, modifier: Modifier = Modifier) {
    val color = when (sensor) {
        SensorType.COIN_D6 -> SensorD6Badge
        SensorType.MID360 -> SensorMid360Badge
    }
    InfoChip(text = sensor.badgeLabel, color = color, modifier = modifier)
}

@Composable
fun ProfileChip(profile: WorkflowProfile, modifier: Modifier = Modifier) {
    InfoChip(text = profile.displayName, color = MaterialTheme.colorScheme.tertiary, modifier = modifier)
}
