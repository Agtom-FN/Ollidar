package com.lidarscan.app.ui.nav

import android.net.Uri
import androidx.compose.runtime.Composable
import androidx.navigation.NavHostController
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.calib.MountCalibrationRoute
import com.lidarscan.app.ui.capture.CaptureRoute
import com.lidarscan.app.ui.connect.ConnectWizardRoute
import com.lidarscan.app.ui.detail.ProjectDetailRoute
import com.lidarscan.app.ui.newproject.NewProjectRoute
import com.lidarscan.app.ui.projects.ProjectsListRoute
import com.lidarscan.app.ui.settings.SettingsRoute

@Composable
fun LidarScanApp(
    container: AppContainer,
    navController: NavHostController = rememberNavController(),
) {
    NavHost(navController = navController, startDestination = Routes.PROJECTS) {
        composable(Routes.PROJECTS) {
            ProjectsListRoute(
                container = container,
                onOpenProject = { projectId -> navController.navigate(Routes.projectDetail(projectId)) },
                onNewProject = { navController.navigate(Routes.NEW_PROJECT) },
                onSettings = { navController.navigate(Routes.SETTINGS) },
            )
        }

        composable(Routes.NEW_PROJECT) {
            NewProjectRoute(
                container = container,
                onCreated = { projectId ->
                    navController.navigate(Routes.projectDetail(projectId)) {
                        popUpTo(Routes.PROJECTS)
                    }
                },
                onCancel = { navController.popBackStack() },
            )
        }

        composable(
            route = Routes.PROJECT_DETAIL,
            arguments = listOf(navArgument(Routes.PROJECT_ID_ARG) { type = NavType.StringType }),
        ) { backStackEntry ->
            val encodedId = backStackEntry.arguments?.getString(Routes.PROJECT_ID_ARG).orEmpty()
            ProjectDetailRoute(
                container = container,
                projectId = Uri.decode(encodedId),
                onBack = { navController.popBackStack() },
                onOpenCapture = { pid -> navController.navigate(Routes.capture(pid)) },
                onOpenMountCalibration = { pid -> navController.navigate(Routes.mountCalibration(pid)) },
            )
        }

        // B7: the mount-calibration wizard (S6 WIZARD.md's five screens).
        composable(
            route = Routes.MOUNT_CALIBRATION,
            arguments = listOf(navArgument(Routes.PROJECT_ID_ARG) { type = NavType.StringType }),
        ) { backStackEntry ->
            val encodedId = backStackEntry.arguments?.getString(Routes.PROJECT_ID_ARG).orEmpty()
            MountCalibrationRoute(
                container = container,
                projectId = Uri.decode(encodedId),
                onBack = { navController.popBackStack() },
            )
        }

        composable(Routes.SETTINGS) {
            SettingsRoute(
                container = container,
                onBack = { navController.popBackStack() },
                onReplaySyntheticCapture = { projectId -> navController.navigate(Routes.replayCapture(projectId)) },
            )
        }

        composable(
            route = Routes.CAPTURE,
            arguments = listOf(navArgument(Routes.PROJECT_ID_ARG) { type = NavType.StringType }),
        ) { backStackEntry ->
            val encodedId = backStackEntry.arguments?.getString(Routes.PROJECT_ID_ARG).orEmpty()
            CaptureRoute(
                container = container,
                projectId = Uri.decode(encodedId),
                onBack = { navController.popBackStack() },
                onConnectDevice = { navController.navigate(Routes.CONNECT_WIZARD) },
            )
        }

        // B4: "Replay synthetic capture" debug-drawer acceptance path — same
        // CaptureRoute, isReplay = true (backed by ReplayEngineBridge, no
        // connect wizard involved).
        composable(
            route = Routes.REPLAY_CAPTURE,
            arguments = listOf(navArgument(Routes.PROJECT_ID_ARG) { type = NavType.StringType }),
        ) { backStackEntry ->
            val encodedId = backStackEntry.arguments?.getString(Routes.PROJECT_ID_ARG).orEmpty()
            CaptureRoute(
                container = container,
                projectId = Uri.decode(encodedId),
                isReplay = true,
                onBack = { navController.popBackStack() },
                onConnectDevice = {},
            )
        }

        composable(Routes.CONNECT_WIZARD) {
            ConnectWizardRoute(
                container = container,
                onBack = { navController.popBackStack() },
                onConnected = { navController.popBackStack() },
            )
        }
    }
}
