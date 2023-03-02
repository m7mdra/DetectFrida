package com.darvin.security;

public class DetectFrida {
    static {
        System.loadLibrary("native-lib");
    }


    public static native void run() throws NullPointerException;

}
