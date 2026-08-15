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
import com.lidarscan.app.ui.connect.Mid360ConnectRoute
import com.lidarscan.app.ui.detail.ProjectDetailRoute
import com.lidarscan.app.ui.merge.MergeRoute
import com.lidarscan.app.ui.newproject.NewProjectRoute
import com.lidarscan.app.ui.plan.PlanRoute
import com.lidarscan.app.ui.processing.ProcessingRoute
import com.lidarscan.app.ui.projects.ProjectsListRoute
import com.lidarscan.app.ui.review.ReviewRoute
import com.lidarscan.app.ui.rtk.RtkRoute
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
                onOpenMid360Connect = { pid -> navController.navigate(Routes.mid360Connect(pid)) },
                onOpenProcessing = { pid -> navController.navigate(Routes.processing(pid)) },
                onOpenReview = { pid -> navController.navigate(Routes.review(pid)) },
                onOpenRtk = { navController.navigate(Routes.RTK) },
                onOpenMerge = { navController.navigate(Routes.MERGE) },
            )
        }

        // B6: Processing — mode chooser (§3.8) + A15's queue.
        composable(
            route = Routes.PROCESSING,
            arguments = listOf(navArgument(Routes.PROJECT_ID_ARG) { type = NavType.StringType }),
        ) { backStackEntry ->
            ProcessingRoute(
                container = container,
                projectId = Uri.decode(backStackEntry.arguments?.getString(Routes.PROJECT_ID_ARG).orEmpty()),
                onBack = { navController.popBackStack() },
                onOpenSettings = { navController.navigate(Routes.SETTINGS) },
            )
        }

        // B10 + B11: Review — viewer, display params, measure; floor plan next door.
        composable(
            route = Routes.REVIEW,
            arguments = listOf(navArgument(Routes.PROJECT_ID_ARG) { type = NavType.StringType }),
        ) { backStackEntry ->
            ReviewRoute(
                container = container,
                projectId = Uri.decode(backStackEntry.arguments?.getString(Routes.PROJECT_ID_ARG).orEmpty()),
                onBack = { navController.popBackStack() },
                onOpenPlan = { pid -> navController.navigate(Routes.plan(pid)) },
            )
        }

        composable(
            route = Routes.PLAN,
            arguments = listOf(navArgument(Routes.PROJECT_ID_ARG) { type = NavType.StringType }),
        ) { backStackEntry ->
            PlanRoute(
                container = container,
                projectId = Uri.decode(backStackEntry.arguments?.getString(Routes.PROJECT_ID_ARG).orEmpty()),
                onBack = { navController.popBackStack() },
            )
        }

        // B9: the rover, the caster and the fix — device-level, not per project.
        composable(Routes.RTK) {
            RtkRoute(container = container, onBack = { navController.popBackStack() })
        }

        // B12: georeferenced auto-merge across projects.
        composable(Routes.MERGE) {
            MergeRoute(container = container, onBack = { navController.popBackStack() })
        }

        // B3: the Mid-360 (Ethernet) connect wizard, per project — this is
        // the entry point that can save the addresses into the project's
        // manifest (Tech Spec 3.1's "Save per project").
        composable(
            route = Routes.MID360_CONNECT,
            arguments = listOf(navArgument(Routes.PROJECT_ID_ARG) { type = NavType.StringType }),
        ) { backStackEntry ->
            val encodedId = backStackEntry.arguments?.getString(Routes.PROJECT_ID_ARG).orEmpty()
            val pid = Uri.decode(encodedId)
            Mid360ConnectRoute(
                container = container,
                projectId = pid,
                onBack = { navController.popBackStack() },
                onContinueToCapture = { navController.navigate(Routes.capture(pid)) },
            )
        }

        // Same wizard with no project behind it — reachable from the generic
        // connect wizard, where it is purely a transport check and there is
        // nothing to save into.
        composable(Routes.MID360_CONNECT_NO_PROJECT) {
            Mid360ConnectRoute(
                container = container,
                projectId = null,
                onBack = { navController.popBackStack() },
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
            val pid = Uri.decode(encodedId)
            CaptureRoute(
                container = container,
                projectId = pid,
                onBack = { navController.popBackStack() },
                onConnectDevice = { navController.navigate(Routes.CONNECT_WIZARD) },
                onOpenMid360Connect = { navController.navigate(Routes.mid360Connect(pid)) },
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
                onOpenMid360 = { navController.navigate(Routes.MID360_CONNECT_NO_PROJECT) },
            )
        }
    }
}
