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
import androidx.lifecycle.compose.collectAsStateWithLifecycle
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
import com.lidarscan.app.ui.profile.ProfileRoute
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

    // ── ROUND 22 item 97: Simple mode, read once and asked everywhere ───────
    //
    // `SimpleMode` (in :core) is the single place that decides what each
    // surface shows; this is the one place the SWITCH is read. Default false —
    // and the default is the feature, because an operator who never opens
    // Settings gets the simple app.
    val settings by container.settingsRepository.settings
        .collectAsStateWithLifecycle(initialValue = com.lidarscan.app.data.AppSettings())
    val advanced = settings.advancedFeatures

    fun openProject(projectId: String) {
        activeProjectId = projectId
        navController.navigate(Routes.projectDetail(projectId))
    }

    fun goTab(route: String) {
        val spec = tabNavSpec(startRoute = Routes.PROJECTS, target = route)
        navController.navigate(route) {
            // ROUND 22 item 88 — see [tabNavSpec] for the whole defect. The
            // three lines below are the standard bottom-tab contract; what was
            // here before was the first line only, and it destroyed the Scan
            // tab's ViewModelStore on every seal.
            popUpTo(spec.popUpToRoute) {
                inclusive = spec.popUpToInclusive
                saveState = spec.saveState
            }
            launchSingleTop = spec.launchSingleTop
            restoreState = spec.restoreState
        }
    }

    Box(Modifier.fillMaxSize()) {
        NavHost(navController = navController, startDestination = Routes.PROJECTS) {
            composable(Routes.PROJECTS) {
                ProjectsListRoute(
                    container = container,
                    // ROUND 8 (item 31): Stop -> seal -> here, with the scan
                    // that was just recorded already selected and previewing.
                    // `activeProjectId` is what CaptureRoute's onScanSealed
                    // sets, so this is the same one-shot handoff, read at the
                    // other end.
                    initialSelectedId = activeProjectId,
                    onSelectProject = { activeProjectId = it },
                    onOpenProject = ::openProject,
                    onOpenReview = { navController.navigate(Routes.review(it)) },
                    // ROUND 22 item 96: ⋯ › Export opens the scan, where the
                    // export row lives.
                    onExport = { navController.navigate(Routes.review(it)) },
                    advanced = advanced,
                    // ROUND 5 (item 8): Projects does not create scans any more —
                    // it points at the tab that does.
                    onNewScan = { goTab(Routes.CAPTURE_NEW) },
                    // ROUND 24 item 109: the avatar opens PROFILE now. It used
                    // to open Settings — which is a tab, three centimetres
                    // below it, with its own icon in the bar. Two doors to one
                    // room, and the room was not the one the avatar implied.
                    onSettings = { navController.navigate(Routes.PROFILE) },
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
                    // ── ROUND 23 item 106(c) ────────────────────────────────
                    //
                    // The owner tests Mid-360 + RTK next, and with Advanced OFF
                    // every door to those two screens was inside the Details
                    // hub that Simple mode hides. `SimpleMode.showsMid360Connect`
                    // / `showsRtk` have said since round 22 that these are
                    // CONTEXTUAL on the sensor rather than gated by the switch;
                    // this is the first surface that actually asks them. The
                    // Scan screen shows the two chips only when a Mid-360 is
                    // the selected sensor (or Advanced is on) — a D6 operator
                    // never sees either.
                    advanced = advanced,
                    onOpenMid360Setup = { navController.navigate(Routes.MID360_SETUP) },
                    onOpenRtk = { navController.navigate(Routes.RTK) },
                    // ── ROUND 8, owner item 31: stop => seal => Projects ──────
                    //
                    // A capture used to end on the Capture tab, still drawing
                    // the cloud it had just finished, with the scan itself
                    // reachable only by remembering to tap Projects. Both of the
                    // owner's lost field sessions were *discovered* in the
                    // Projects tab, which is the tab that answers the question a
                    // finished capture leaves you with — "is it saved?" — so it
                    // is where a finished capture lands.
                    //
                    // `goTab` pops back to the graph's root, so this does not
                    // grow the back stack: the Capture tab's own
                    // `NavBackStackEntry` and ViewModel are destroyed, and
                    // returning to Capture builds a fresh one. That is the same
                    // trip ROUND 7's field bug 1 was about — and the reason the
                    // mount trim is persisted rather than held in the ViewModel
                    // is precisely so this navigation is safe to do
                    // automatically now. `CaptureViewModel` re-arms everything
                    // else (fresh auto-name, cleared typed name, zeroed stats)
                    // before it emits, and the connection is untouched.
                    onScanSealed = { pid ->
                        activeProjectId = pid
                        goTab(Routes.PROJECTS)
                    },
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
                // ROUND 19 item 76: the SAME chrome from both doors. Review is
                // reachable from the Projects list and from Project detail;
                // it used to be the one project screen NOT wrapped in
                // UnderTabBar, so arriving from Detail (which is wrapped)
                // visibly re-flowed the page under the tab bar — the
                // "different chrome" round 16 named. Both routes stay (the
                // seal->Projects handoff needs the list door, and removing a
                // navigation route mid-round is the back-stack risk round 16
                // called out); what is consolidated is the chrome: one
                // wrapper, like every sibling project screen.
                UnderTabBar {
                    ReviewRoute(
                        container = container,
                        projectId = Uri.decode(backStackEntry.arguments?.getString(Routes.PROJECT_ID_ARG).orEmpty()),
                        onBack = { navController.popBackStack() },
                        // ROUND 22 item 97: the floor plan is an Advanced
                        // feature. Null hides the pill AND makes the route
                        // unreachable from here, which is the honest pairing —
                        // a hidden door with a live route behind it is how a
                        // half-removed feature comes back through a deep link.
                        onOpenPlan = if (com.lidarscan.core.SimpleMode.showsFloorPlan(advanced)) {
                            { pid: String -> navController.navigate(Routes.plan(pid)) }
                        } else {
                            null
                        },
                        // ROUND 22 item 96: with Advanced ON, Export still
                        // opens the full Processing screen. In Simple mode
                        // Review carries its own export row (reusing
                        // ProcessingViewModel's paths verbatim), so there is
                        // nowhere to send the operator.
                        onOpenExport = if (com.lidarscan.core.SimpleMode.showsProjectDetailHub(advanced)) {
                            { pid: String ->
                                activeProjectId = pid
                                navController.navigate(Routes.processing(pid))
                            }
                        } else {
                            null
                        },
                        advanced = advanced,
                    )
                }
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


            // ROUND 23 item 106(c): the same wizard, with no project behind it,
            // reachable from the Scan tab so a Mid-360 can be set up before the
            // first scan exists.
            composable(Routes.MID360_SETUP) {
                UnderTabBar {
                    Mid360ConnectRoute(
                        container = container,
                        projectId = null,
                        onBack = { navController.popBackStack() },
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

            // ROUND 24 item 109: the Profile page — device/app facts, Send
            // logs and Feedback. Wrapped like every other secondary screen so
            // its last control does not sit under the floating tab bar.
            composable(Routes.PROFILE) {
                UnderTabBar {
                    ProfileRoute(container = container, onBack = { navController.popBackStack() })
                }
            }

            composable(Routes.SETTINGS) {
                SettingsRoute(
                    container = container,
                    onBack = { goTab(Routes.PROJECTS) },
                    // ROUND 24 items 109 + 113: Settings' own Profile row, at
                    // the top, so the page is reachable from the tab as well
                    // as from the avatar.
                    onOpenProfile = { navController.navigate(Routes.PROFILE) },
                    // ROUND 24 item 110(b): a tour of the Scan screen has to
                    // run on the Scan screen. Settings arms the one-shot and
                    // hops to the tab; CaptureRoute honours it and clears it.
                    onReplayTutorial = {
                        container.tutorialReplayRequest.value = true
                        goTab(Routes.CAPTURE_NEW)
                    },
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
