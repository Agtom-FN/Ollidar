package com.lidarscan.app.ui.nav

import android.net.Uri
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.navigation.NavHostController
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.lidarscan.app.di.AppContainer
import com.lidarscan.app.ui.components.ScanDims
import com.lidarscan.app.ui.calib.MountCalibrationRoute
import com.lidarscan.app.ui.capture.CaptureRoute
import com.lidarscan.app.ui.connect.ConnectWizardRoute
import com.lidarscan.app.ui.connect.Mid360ConnectRoute
import com.lidarscan.app.ui.detail.ProjectDetailRoute
import com.lidarscan.app.ui.merge.MergeRoute
import com.lidarscan.app.ui.newproject.NewProjectRoute
import com.lidarscan.app.ui.pick.PickPurpose
import com.lidarscan.app.ui.pick.ProjectPickerRoute
import com.lidarscan.app.ui.plan.PlanRoute
import com.lidarscan.app.ui.processing.ProcessingRoute
import com.lidarscan.app.ui.projects.ProjectsListRoute
import com.lidarscan.app.ui.review.ReviewRoute
import com.lidarscan.app.ui.rtk.RtkRoute
import com.lidarscan.app.ui.settings.SettingsRoute

/**
 * The redesign's navigation shell: one `NavHost` under one floating capsule
 * tab bar.
 *
 * **What changed and why.** Capture and Jobs used to live only behind a
 * project's detail screen. The tab bar promotes them, which needs one piece of
 * state the old graph did not have — *which* project a bare "Capture" tap
 * means. That is [activeProjectId]: set whenever the user opens a project,
 * starts a capture or opens a queue, and read by the tab bar to pick between
 * `project/{id}/capture` and the [Routes.CAPTURE_PICK] picker. It is
 * `rememberSaveable`, so it survives rotation and back-stack churn but not
 * process death — after a cold start the first Capture tap lands on the picker,
 * which is the honest answer rather than resuming a project the user may not
 * have meant.
 *
 * Secondary screens (detail, review, plan, merge, RTK, the wizards, new
 * project) keep their own back arrow and light their parent tab, per
 * [tabForRoute].
 */
@Composable
fun LidarScanApp(
    container: AppContainer,
    navController: NavHostController = rememberNavController(),
) {
    var activeProjectId by rememberSaveable { mutableStateOf<String?>(null) }
    val currentEntry by navController.currentBackStackEntryAsState()
    val currentTab = tabForRoute(currentEntry?.destination?.route)

    fun openProject(projectId: String) {
        activeProjectId = projectId
        navController.navigate(Routes.projectDetail(projectId))
    }

    fun goTab(route: String) {
        navController.navigate(route) {
            // Every tab is a sibling of Projects, not a child of whatever
            // screen happened to be on top: pop back to the graph's root
            // before landing so the back stack never grows one entry per tab
            // tap. `inclusive` only for the Projects tab itself, which is that
            // root.
            popUpTo(Routes.PROJECTS) { inclusive = route == Routes.PROJECTS }
            launchSingleTop = true
        }
    }

    Box(Modifier.fillMaxSize()) {
        NavHost(navController = navController, startDestination = Routes.PROJECTS) {
            composable(Routes.PROJECTS) {
                ProjectsListRoute(
                    container = container,
                    onOpenProject = ::openProject,
                    onNewProject = { navController.navigate(Routes.NEW_PROJECT) },
                    onSettings = { goTab(Routes.SETTINGS) },
                )
            }

            // Capture as a top-level tab with no active project: pick one, or
            // start a new scan. Choosing here also sets the active project, so
            // the next Capture tap goes straight to the viewport.
            composable(Routes.CAPTURE_PICK) {
                ProjectPickerRoute(
                    container = container,
                    purpose = PickPurpose.CAPTURE,
                    onPick = { pid ->
                        activeProjectId = pid
                        navController.navigate(Routes.capture(pid)) {
                            popUpTo(Routes.CAPTURE_PICK) { inclusive = true }
                        }
                    },
                    onNewProject = { navController.navigate(Routes.NEW_PROJECT) },
                    onBack = { goTab(Routes.PROJECTS) },
                )
            }

            composable(Routes.JOBS_PICK) {
                ProjectPickerRoute(
                    container = container,
                    purpose = PickPurpose.JOBS,
                    onPick = { pid ->
                        activeProjectId = pid
                        navController.navigate(Routes.processing(pid)) {
                            popUpTo(Routes.JOBS_PICK) { inclusive = true }
                        }
                    },
                    onNewProject = { navController.navigate(Routes.NEW_PROJECT) },
                    onBack = { goTab(Routes.PROJECTS) },
                )
            }

            composable(Routes.NEW_PROJECT) {
                UnderTabBar {
                    NewProjectRoute(
                        container = container,
                        onCreated = { projectId ->
                            activeProjectId = projectId
                            navController.navigate(Routes.projectDetail(projectId)) {
                                popUpTo(Routes.PROJECTS)
                            }
                        },
                        onCancel = { navController.popBackStack() },
                    )
                }
            }

            composable(
                route = Routes.PROJECT_DETAIL,
                arguments = listOf(navArgument(Routes.PROJECT_ID_ARG) { type = NavType.StringType }),
            ) { backStackEntry ->
                val encodedId = backStackEntry.arguments?.getString(Routes.PROJECT_ID_ARG).orEmpty()
                val pid = Uri.decode(encodedId)
                UnderTabBar {
                    ProjectDetailRoute(
                        container = container,
                        projectId = pid,
                        onBack = { navController.popBackStack() },
                        onOpenCapture = {
                            activeProjectId = it
                            navController.navigate(Routes.capture(it))
                        },
                        onOpenMountCalibration = { navController.navigate(Routes.mountCalibration(it)) },
                        onOpenMid360Connect = { navController.navigate(Routes.mid360Connect(it)) },
                        onOpenProcessing = {
                            activeProjectId = it
                            navController.navigate(Routes.processing(it))
                        },
                        onOpenReview = { navController.navigate(Routes.review(it)) },
                        onOpenRtk = { navController.navigate(Routes.RTK) },
                        onOpenMerge = { navController.navigate(Routes.MERGE) },
                    )
                }
            }

            // B6: Processing — mode chooser (§3.8) + A15's queue. Now also the
            // Jobs tab's destination once a project is active.
            composable(
                route = Routes.PROCESSING,
                arguments = listOf(navArgument(Routes.PROJECT_ID_ARG) { type = NavType.StringType }),
            ) { backStackEntry ->
                ProcessingRoute(
                    container = container,
                    projectId = Uri.decode(backStackEntry.arguments?.getString(Routes.PROJECT_ID_ARG).orEmpty()),
                    onBack = { navController.popBackStack() },
                    onOpenSettings = { goTab(Routes.SETTINGS) },
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
                    onOpenExport = { pid ->
                        activeProjectId = pid
                        navController.navigate(Routes.processing(pid))
                    },
                )
            }

            composable(
                route = Routes.PLAN,
                arguments = listOf(navArgument(Routes.PROJECT_ID_ARG) { type = NavType.StringType }),
            ) { backStackEntry ->
                UnderTabBar {
                    PlanRoute(
                        container = container,
                        projectId = Uri.decode(backStackEntry.arguments?.getString(Routes.PROJECT_ID_ARG).orEmpty()),
                        onBack = { navController.popBackStack() },
                    )
                }
            }

            // B9: the rover, the caster and the fix — device-level, not per project.
            composable(Routes.RTK) {
                UnderTabBar {
                    RtkRoute(container = container, onBack = { navController.popBackStack() })
                }
            }

            // B12: georeferenced auto-merge across projects.
            composable(Routes.MERGE) {
                UnderTabBar {
                    MergeRoute(container = container, onBack = { navController.popBackStack() })
                }
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
                UnderTabBar {
                    Mid360ConnectRoute(
                        container = container,
                        projectId = pid,
                        onBack = { navController.popBackStack() },
                        onContinueToCapture = {
                            activeProjectId = pid
                            navController.navigate(Routes.capture(pid))
                        },
                    )
                }
            }

            // Same wizard with no project behind it — reachable from the generic
            // connect wizard, where it is purely a transport check and there is
            // nothing to save into.
            composable(Routes.MID360_CONNECT_NO_PROJECT) {
                UnderTabBar {
                    Mid360ConnectRoute(
                        container = container,
                        projectId = null,
                        onBack = { navController.popBackStack() },
                    )
                }
            }

            // B7: the mount-calibration wizard (S6 WIZARD.md's five screens).
            composable(
                route = Routes.MOUNT_CALIBRATION,
                arguments = listOf(navArgument(Routes.PROJECT_ID_ARG) { type = NavType.StringType }),
            ) { backStackEntry ->
                val encodedId = backStackEntry.arguments?.getString(Routes.PROJECT_ID_ARG).orEmpty()
                UnderTabBar {
                    MountCalibrationRoute(
                        container = container,
                        projectId = Uri.decode(encodedId),
                        onBack = { navController.popBackStack() },
                    )
                }
            }

            composable(Routes.SETTINGS) {
                SettingsRoute(
                    container = container,
                    onBack = { goTab(Routes.PROJECTS) },
                    onReplaySyntheticCapture = { projectId ->
                        activeProjectId = projectId
                        navController.navigate(Routes.replayCapture(projectId))
                    },
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
                UnderTabBar {
                    ConnectWizardRoute(
                        container = container,
                        onBack = { navController.popBackStack() },
                        onConnected = { navController.popBackStack() },
                        onOpenMid360 = { navController.navigate(Routes.MID360_CONNECT_NO_PROJECT) },
                    )
                }
            }
        }

        ScanTabBar(
            current = currentTab,
            onSelect = { tab ->
                val target = when (tab) {
                    ScanTab.PROJECTS -> Routes.PROJECTS
                    ScanTab.CAPTURE -> activeProjectId?.let { Routes.capture(it) } ?: Routes.CAPTURE_PICK
                    ScanTab.JOBS -> activeProjectId?.let { Routes.processing(it) } ?: Routes.JOBS_PICK
                    ScanTab.SETTINGS -> Routes.SETTINGS
                }
                goTab(target)
            },
            modifier = Modifier.align(Alignment.BottomCenter).navigationBarsPadding(),
        )
    }
}

/**
 * Bottom clearance for the screens the redesign did **not** restyle.
 *
 * The tab bar floats over the content rather than docking under it, so any
 * screen that scrolls to its own bottom edge would end with its last control
 * under the capsule. The redesigned screens each reserve
 * `ScanDims.TabBarClearance` in their own content padding (the mockup does the
 * same thing with its `.haspad` rule); the wizards, the project detail screen,
 * the plan/RTK/merge screens and the new-project flow still use Material
 * `Scaffold`s from B1–B12 and get it here instead — one wrapper rather than a
 * restyle each, which is the honest scope line for this task.
 */
@Composable
private fun UnderTabBar(content: @Composable () -> Unit) {
    Box(
        Modifier
            .fillMaxSize()
            .navigationBarsPadding()
            .padding(bottom = ScanDims.TabBarClearance),
    ) { content() }
}
