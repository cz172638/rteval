import sys, os, readline, libxml2
from rteval.Log import Log

class cmdlineInfo:
    def __init__(self, logger = None):
        self.__logger = logger

    def __log(self, logtype, msg):
        if self.__logger:
            self.__logger.log(logtype, msg)

    def read_cmdline(self):
        cmdlineList = []
        fp = open('/proc/cmdline', 'r')
        line = fp.readline()
        self.__log(Log.DEBUG, "/proc/mcdline\n")
        fp.close()
        return line

    def MakeReport(self):
        rep_n = libxml2.newNode("cmdlineInfo")
        cmdline_n = libxml2.newNode("cmdline")
        cmdlineStr = self.read_cmdline()
        cmdline_n.addContent(cmdlineStr)
        self.__log(Log.DEBUG, cmdlineStr)
        rep_n.addChild(cmdline_n)

        return rep_n
