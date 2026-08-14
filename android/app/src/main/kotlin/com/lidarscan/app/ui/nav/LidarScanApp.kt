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
            )
        }

        composable(Routes.SETTINGS) {
            SettingsRoute(
                container = container,
                onBack = { navController.popBackStack() },
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

        composable(Routes.CONNECT_WIZARD) {
            ConnectWizardRoute(
                container = container,
                onBack = { navController.popBackStack() },
                onConnected = { navController.popBackStack() },
            )
        }
    }
}
