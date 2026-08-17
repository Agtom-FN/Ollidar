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
import com.lidarscan.app.ui.connect.Mid360ConnectRoute
import com.lidarscan.app.ui.detail.ProjectDetailRoute
import com.lidarscan.app.ui.merge.MergeRoute
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
 * **ROUND 5's tab roles** (items 8 + 9), which is what this graph now encodes:
 *
 *  * **Projects** — the list, and a preview of the selected scan. It never
 *    starts a capture; selecting a card sets [activeProjectId] (for the Jobs tab)
 *    and opens the inline preview.
 *  * **Capture** — creating new scan projects, and nothing else. One destination
 *    ([Routes.CAPTURE_NEW]) with no project id and no picker in front of it,
 *    because Start is what creates the project.
 *  * **Jobs** — still per project (a queue is a queue *for* a project), so it
 *    keeps [Routes.JOBS_PICK] for the no-active-project case.
 *
 * [activeProjectId] survives rotation and back-stack churn (`rememberSaveable`)
 * but not process death; after a cold start the first Jobs tap lands on the
 * picker, which is the honest answer rather than resuming a project the user may
 * not have meant.
 *
 * Secondary screens (detail, review, plan, merge, RTK, the wizards) keep their own
 * back arrow and light their parent tab, per [tabForRoute].
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
                    onSelectProject = { activeProjectId = it },
                    onOpenProject = ::openProject,
                    onOpenReview = { navController.navigate(Routes.review(it)) },
                    // ROUND 5 (item 8): Projects does not create scans any more —
                    // it points at the tab that does.
                    onNewScan = { goTab(Routes.CAPTURE_NEW) },
                    onSettings = { goTab(Routes.SETTINGS) },
                )
            }

            // ROUND 5 (items 8 + 9): the Capture tab. No project id, no picker —
            // Start creates a new project and records into it.
            composable(Routes.CAPTURE_NEW) {
                CaptureRoute(
                    container = container,
                    projectId = null,
                    onBack = { goTab(Routes.PROJECTS) },
                    onOpenMountCalibration = { pid -> navController.navigate(Routes.mountCalibration(pid)) },
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
                    onNewProject = { goTab(Routes.CAPTURE_NEW) },
                    onBack = { goTab(Routes.PROJECTS) },
                )
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
                        // ROUND 5: no capture door here. A capture always creates a
                        // new project (item 9), so "record more into this one" is
                        // not an action the app offers any more — the Capture tab
                        // is the only way in, and it starts a new scan.
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
                        // ROUND 5: "continue" lands on the Capture tab, which
                        // starts a NEW scan (item 9). The addresses this wizard
                        // just saved are also stored device-level, so the new
                        // project picks them up without being this project.
                        onContinueToCapture = { goTab(Routes.CAPTURE_NEW) },
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
                )
            }

        }

        ScanTabBar(
            current = currentTab,
            onSelect = { tab ->
                val target = when (tab) {
                    ScanTab.PROJECTS -> Routes.PROJECTS
                    // ROUND 5 (item 8): one destination, always. The Capture tab
                    // creates new scans, so there is nothing for an "active
                    // project" to change about where it lands.
                    ScanTab.CAPTURE -> Routes.CAPTURE_NEW
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
