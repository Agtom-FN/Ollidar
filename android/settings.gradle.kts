pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        // B2: mik3y:usb-serial-for-android is JitPack-only, not on Maven
        // Central. Scoped narrowly (not "any group") since FAIL_ON_PROJECT_REPOS
        // funnels every module's dependency resolution through this block.
        maven {
            url = uri("https://jitpack.io")
            content { includeGroup("com.github.mik3y") }
        }
    }
}

rootProject.name = "lidarscan"

include(":app")
include(":core")
