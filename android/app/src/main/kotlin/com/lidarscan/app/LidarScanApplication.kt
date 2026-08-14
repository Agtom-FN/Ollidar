package com.lidarscan.app

import android.app.Application
import com.lidarscan.app.di.AppContainer

class LidarScanApplication : Application() {

    lateinit var container: AppContainer
        private set

    override fun onCreate() {
        super.onCreate()
        container = AppContainer(this)
    }
}
