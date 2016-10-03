'''CTS: Cluster Testing System: Tests module

There are a few things we want to do here:

 '''

__copyright__ = '''
Copyright (C) 2000, 2001 Alan Robertson <alanr@unix.sh>
Licensed under the GNU GPL.

Add RecourceRecover testcase Zhao Kai <zhaokai@cn.ibm.com>
'''

#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA.

#
#        SPECIAL NOTE:
#
#        Tests may NOT implement any cluster-manager-specific code in them.
#        EXTEND the ClusterManager object to provide the base capabilities
#        the test needs if you need to do something that the current CM classes
#        do not.  Otherwise you screw up the whole point of the object structure
#        in CTS.
#
#                Thank you.
#

import time, os, re, string, subprocess, tempfile
from stat import *
from cts import CTS
from cts.CTSaudits import *
from cts.CTSvars   import *
from cts.patterns  import PatternSelector
from cts.logging   import LogFactory
from cts.remote    import RemoteFactory
from cts.watcher   import LogWatcher
from cts.environment import EnvFactory

AllTestClasses = [ ]


class CTSTest:
    '''
    A Cluster test.
    We implement the basic set of properties and behaviors for a generic
    cluster test.

    Cluster tests track their own statistics.
    We keep each of the kinds of counts we track as separate {name,value}
    pairs.
    '''

    def __init__(self, cm):
        #self.name="the unnamed test"
        self.Stats = {"calls":0
        ,        "success":0
        ,        "failure":0
        ,        "skipped":0
        ,        "auditfail":0}

#        if not issubclass(cm.__class__, ClusterManager):
#            raise ValueError("Must be a ClusterManager object")
        self.CM = cm
        self.Env = EnvFactory().getInstance()
        self.rsh = RemoteFactory().getInstance()
        self.logger = LogFactory()
        self.templates = PatternSelector(cm["Name"])
        self.Audits = []
        self.timeout = 120
        self.passed = 1
        self.is_loop = 0
        self.is_unsafe = 0
        self.is_docker_unsafe = 0
        self.is_experimental = 0
        self.is_container = 0
        self.is_valgrind = 0
        self.benchmark = 0  # which tests to benchmark
        self.timer = {}  # timers

    def log(self, args):
        self.logger.log(args)

    def debug(self, args):
        self.logger.debug(args)

    def has_key(self, key):
        return key in self.Stats

    def __setitem__(self, key, value):
        self.Stats[key] = value

    def __getitem__(self, key):
        if str(key) == "0":
            raise ValueError("Bad call to 'foo in X', should reference 'foo in X.Stats' instead")

        if key in self.Stats:
            return self.Stats[key]
        return None

    def log_mark(self, msg):
        self.debug("MARK: test %s %s %d" % (self.name,msg,time.time()))
        return

    def get_timer(self,key = "test"):
        try: return self.timer[key]
        except: return 0

    def set_timer(self,key = "test"):
        self.timer[key] = time.time()
        return self.timer[key]

    def log_timer(self,key = "test"):
        elapsed = 0
        if key in self.timer:
            elapsed = time.time() - self.timer[key]
            s = key == "test" and self.name or "%s:%s" % (self.name,key)
            self.debug("%s runtime: %.2f" % (s, elapsed))
            del self.timer[key]
        return elapsed

    def incr(self, name):
        '''Increment (or initialize) the value associated with the given name'''
        if not name in self.Stats:
            self.Stats[name] = 0
        self.Stats[name] = self.Stats[name]+1

        # Reset the test passed boolean
        if name == "calls":
            self.passed = 1

    def failure(self, reason="none"):
        '''Increment the failure count'''
        self.passed = 0
        self.incr("failure")
        self.logger.log(("Test %s" % self.name).ljust(35) + " FAILED: %s" % reason)
        return None

    def success(self):
        '''Increment the success count'''
        self.incr("success")
        return 1

    def skipped(self):
        '''Increment the skipped count'''
        self.incr("skipped")
        return 1

    def __call__(self, node):
        '''Perform the given test'''
        raise ValueError("Abstract Class member (__call__)")
        self.incr("calls")
        return self.failure()

    def audit(self):
        passed = 1
        if len(self.Audits) > 0:
            for audit in self.Audits:
                if not audit():
                    self.logger.log("Internal %s Audit %s FAILED." % (self.name, audit.name()))
                    self.incr("auditfail")
                    passed = 0
        return passed

    def setup(self, node):
        '''Setup the given test'''
        return self.success()

    def teardown(self, node):
        '''Tear down the given test'''
        return self.success()

    def create_watch(self, patterns, timeout, name=None):
        if not name:
            name = self.name
        return LogWatcher(self.Env["LogFileName"], patterns, name, timeout, kind=self.Env["LogWatcher"], hosts=self.Env["nodes"])

    def local_badnews(self, prefix, watch, local_ignore=[]):
        errcount = 0
        if not prefix:
            prefix = "LocalBadNews:"

        ignorelist = []
        ignorelist.append(" CTS: ")
        ignorelist.append(prefix)
        ignorelist.extend(local_ignore)

        while errcount < 100:
            match = watch.look(0)
            if match:
               add_err = 1
               for ignore in ignorelist:
                   if add_err == 1 and re.search(ignore, match):
                       add_err = 0
               if add_err == 1:
                   self.logger.log(prefix + " " + match)
                   errcount = errcount + 1
            else:
              break
        else:
            self.logger.log("Too many errors!")

        watch.end()
        return errcount

    def is_applicable(self):
        return self.is_applicable_common()

    def is_applicable_common(self):
        '''Return TRUE if we are applicable in the current test configuration'''
        #raise ValueError("Abstract Class member (is_applicable)")

        if self.is_loop and not self.Env["loop-tests"]:
            return 0
        elif self.is_unsafe and not self.Env["unsafe-tests"]:
            return 0
        elif self.is_valgrind and not self.Env["valgrind-tests"]:
            return 0
        elif self.is_experimental and not self.Env["experimental-tests"]:
            return 0
        elif self.is_docker_unsafe and self.Env["docker"]:
            return 0
        elif self.is_container and not self.Env["container-tests"]:
            return 0
        elif self.Env["benchmark"] and self.benchmark == 0:
            return 0

        return 1

    def find_ocfs2_resources(self, node):
        self.r_o2cb = None
        self.r_ocfs2 = []

        (rc, lines) = self.rsh(node, "crm_resource -c", None)
        for line in lines:
            if re.search("^Resource", line):
                r = AuditResource(self.CM, line)
                if r.rtype == "o2cb" and r.parent != "NA":
                    self.debug("Found o2cb: %s" % self.r_o2cb)
                    self.r_o2cb = r.parent
            if re.search("^Constraint", line):
                c = AuditConstraint(self.CM, line)
                if c.type == "rsc_colocation" and c.target == self.r_o2cb:
                    self.r_ocfs2.append(c.rsc)

        self.debug("Found ocfs2 filesystems: %s" % repr(self.r_ocfs2))
        return len(self.r_ocfs2)

    def canrunnow(self, node):
        '''Return TRUE if we can meaningfully run right now'''
        return 1

    def errorstoignore(self):
        '''Return list of errors which are 'normal' and should be ignored'''
        return []


class StopTest(CTSTest):
    '''Stop (deactivate) the cluster manager on a node'''
    def __init__(self, cm):
        CTSTest.__init__(self, cm)
        self.name = "Stop"

    def __call__(self, node):
        '''Perform the 'stop' test. '''
        self.incr("calls")
        if self.CM.ShouldBeStatus[node] != "up":
            return self.skipped()

        patterns = []
        # Technically we should always be able to notice ourselves stopping
        patterns.append(self.templates["Pat:We_stopped"] % node)

        #if self.Env["use_logd"]:
        #    patterns.append(self.templates["Pat:Logd_stopped"] % node)

        # Any active node needs to notice this one left
        # NOTE: This wont work if we have multiple partitions
        for other in self.Env["nodes"]:
            if self.CM.ShouldBeStatus[other] == "up" and other != node:
                patterns.append(self.templates["Pat:They_stopped"] %(other, self.CM.key_for_node(node)))
                #self.debug("Checking %s will notice %s left"%(other, node))

        watch = self.create_watch(patterns, self.Env["DeadTime"])
        watch.setwatch()

        if node == self.CM.OurNode:
            self.incr("us")
        else:
            if self.CM.upcount() <= 1:
                self.incr("all")
            else:
                self.incr("them")

        self.CM.StopaCM(node)
        watch_result = watch.lookforall()

        failreason = None
        UnmatchedList = "||"
        if watch.unmatched:
            (rc, output) = self.rsh(node, "/bin/ps axf", None)
            for line in output:
                self.debug(line)

            (rc, output) = self.rsh(node, "/usr/sbin/dlm_tool dump", None)
            for line in output:
                self.debug(line)

            for regex in watch.unmatched:
                self.logger.log ("ERROR: Shutdown pattern not found: %s" % (regex))
                UnmatchedList +=  regex + "||";
                failreason = "Missing shutdown pattern"

        self.CM.cluster_stable(self.Env["DeadTime"])

        if not watch.unmatched or self.CM.upcount() == 0:
            return self.success()

        if len(watch.unmatched) >= self.CM.upcount():
            return self.failure("no match against (%s)" % UnmatchedList)

        if failreason == None:
            return self.success()
        else:
            return self.failure(failreason)
#
# We don't register StopTest because it's better when called by
# another test...
#


class StartTest(CTSTest):
    '''Start (activate) the cluster manager on a node'''
    def __init__(self, cm, debug=None):
        CTSTest.__init__(self,cm)
        self.name = "start"
        self.debug = debug

    def __call__(self, node):
        '''Perform the 'start' test. '''
        self.incr("calls")

        if self.CM.upcount() == 0:
            self.incr("us")
        else:
            self.incr("them")

        if self.CM.ShouldBeStatus[node] != "down":
            return self.skipped()
        elif self.CM.StartaCM(node):
            return self.success()
        else:
            return self.failure("Startup %s on node %s failed"
                                % (self.Env["Name"], node))

#
# We don't register StartTest because it's better when called by
# another test...
#


class FlipTest(CTSTest):
    '''If it's running, stop it.  If it's stopped start it.
       Overthrow the status quo...
    '''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "Flip"
        self.start = StartTest(cm)
        self.stop = StopTest(cm)

    def __call__(self, node):
        '''Perform the 'Flip' test. '''
        self.incr("calls")
        if self.CM.ShouldBeStatus[node] == "up":
            self.incr("stopped")
            ret = self.stop(node)
            type = "up->down"
            # Give the cluster time to recognize it's gone...
            time.sleep(self.Env["StableTime"])
        elif self.CM.ShouldBeStatus[node] == "down":
            self.incr("started")
            ret = self.start(node)
            type = "down->up"
        else:
            return self.skipped()

        self.incr(type)
        if ret:
            return self.success()
        else:
            return self.failure("%s failure" % type)

#        Register FlipTest as a good test to run
AllTestClasses.append(FlipTest)


class RestartTest(CTSTest):
    '''Stop and restart a node'''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "Restart"
        self.start = StartTest(cm)
        self.stop = StopTest(cm)
        self.benchmark = 1

    def __call__(self, node):
        '''Perform the 'restart' test. '''
        self.incr("calls")

        self.incr("node:" + node)

        ret1 = 1
        if self.CM.StataCM(node):
            self.incr("WasStopped")
            if not self.start(node):
                return self.failure("start (setup) failure: "+node)

        self.set_timer()
        if not self.stop(node):
            return self.failure("stop failure: "+node)
        if not self.start(node):
            return self.failure("start failure: "+node)
        return self.success()

#        Register RestartTest as a good test to run
AllTestClasses.append(RestartTest)


class StonithdTest(CTSTest):
    def __init__(self, cm):
        CTSTest.__init__(self, cm)
        self.name = "Stonithd"
        self.startall = SimulStartLite(cm)
        self.benchmark = 1

    def __call__(self, node):
        self.incr("calls")
        if len(self.Env["nodes"]) < 2:
            return self.skipped()

        ret = self.startall(None)
        if not ret:
            return self.failure("Setup failed")

        is_dc = self.CM.is_node_dc(node)

        watchpats = []
        watchpats.append(self.templates["Pat:FenceOpOK"] % node)
        watchpats.append(self.templates["Pat:NodeFenced"] % node)

        if self.Env["at-boot"] == 0:
            self.debug("Expecting %s to stay down" % node)
            self.CM.ShouldBeStatus[node] = "down"
        else:
            self.debug("Expecting %s to come up again %d" % (node, self.Env["at-boot"]))
            watchpats.append("%s.* S_STARTING -> S_PENDING" % node)
            watchpats.append("%s.* S_PENDING -> S_NOT_DC" % node)

        watch = self.create_watch(watchpats, 30 + self.Env["DeadTime"] + self.Env["StableTime"] + self.Env["StartTime"])
        watch.setwatch()

        origin = self.Env.RandomGen.choice(self.Env["nodes"])

        rc = self.rsh(origin, "stonith_admin --reboot %s -VVVVVV" % node)

        if rc == 194:
            # 194 - 256 = -62 = Timer expired
            #
            # Look for the patterns, usually this means the required
            # device was running on the node to be fenced - or that
            # the required devices were in the process of being loaded
            # and/or moved
            #
            # Effectively the node committed suicide so there will be
            # no confirmation, but pacemaker should be watching and
            # fence the node again

            self.logger.log("Fencing command on %s to fence %s timed out" % (origin, node))

        elif origin != node and rc != 0:
            self.debug("Waiting for the cluster to recover")
            self.CM.cluster_stable()

            self.debug("Waiting STONITHd node to come back up")
            self.CM.ns.WaitForAllNodesToComeUp(self.Env["nodes"], 600)

            self.logger.log("Fencing command on %s failed to fence %s (rc=%d)" % (origin, node, rc))

        elif origin == node and rc != 255:
            # 255 == broken pipe, ie. the node was fenced as expected
            self.logger.log("Locally originated fencing returned %d" % rc)

        self.set_timer("fence")
        matched = watch.lookforall()
        self.log_timer("fence")
        self.set_timer("reform")
        if watch.unmatched:
            self.logger.log("Patterns not found: " + repr(watch.unmatched))

        self.debug("Waiting for the cluster to recover")
        self.CM.cluster_stable()

        self.debug("Waiting STONITHd node to come back up")
        self.CM.ns.WaitForAllNodesToComeUp(self.Env["nodes"], 600)

        self.debug("Waiting for the cluster to re-stabilize with all nodes")
        is_stable = self.CM.cluster_stable(self.Env["StartTime"])

        if not matched:
            return self.failure("Didn't find all expected patterns")
        elif not is_stable:
            return self.failure("Cluster did not become stable")

        self.log_timer("reform")
        return self.success()

    def errorstoignore(self):
        return [
            self.templates["Pat:Fencing_start"] % ".*",
            self.templates["Pat:Fencing_ok"] % ".*",
            r"error.*: Resource .*stonith::.* is active on 2 nodes attempting recovery",
            r"error.*: Operation reboot of .*by .* for stonith_admin.*: Timer expired",
        ]

    def is_applicable(self):
        if not self.is_applicable_common():
            return 0

        if "DoFencing" in self.Env.keys():
            return self.Env["DoFencing"]

        return 1

AllTestClasses.append(StonithdTest)


class StartOnebyOne(CTSTest):
    '''Start all the nodes ~ one by one'''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "StartOnebyOne"
        self.stopall = SimulStopLite(cm)
        self.start = StartTest(cm)
        self.ns = CTS.NodeStatus(cm.Env)

    def __call__(self, dummy):
        '''Perform the 'StartOnebyOne' test. '''
        self.incr("calls")

        #        We ignore the "node" parameter...

        #        Shut down all the nodes...
        ret = self.stopall(None)
        if not ret:
            return self.failure("Test setup failed")

        failed = []
        self.set_timer()
        for node in self.Env["nodes"]:
            if not self.start(node):
                failed.append(node)

        if len(failed) > 0:
            return self.failure("Some node failed to start: " + repr(failed))

        return self.success()

#        Register StartOnebyOne as a good test to run
AllTestClasses.append(StartOnebyOne)


class SimulStart(CTSTest):
    '''Start all the nodes ~ simultaneously'''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "SimulStart"
        self.stopall = SimulStopLite(cm)
        self.startall = SimulStartLite(cm)

    def __call__(self, dummy):
        '''Perform the 'SimulStart' test. '''
        self.incr("calls")

        #        We ignore the "node" parameter...

        #        Shut down all the nodes...
        ret = self.stopall(None)
        if not ret:
            return self.failure("Setup failed")

        self.CM.clear_all_caches()

        if not self.startall(None):
            return self.failure("Startall failed")

        return self.success()

#        Register SimulStart as a good test to run
AllTestClasses.append(SimulStart)


class SimulStop(CTSTest):
    '''Stop all the nodes ~ simultaneously'''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "SimulStop"
        self.startall = SimulStartLite(cm)
        self.stopall = SimulStopLite(cm)

    def __call__(self, dummy):
        '''Perform the 'SimulStop' test. '''
        self.incr("calls")

        #     We ignore the "node" parameter...

        #     Start up all the nodes...
        ret = self.startall(None)
        if not ret:
            return self.failure("Setup failed")

        if not self.stopall(None):
            return self.failure("Stopall failed")

        return self.success()

#     Register SimulStop as a good test to run
AllTestClasses.append(SimulStop)


class StopOnebyOne(CTSTest):
    '''Stop all the nodes in order'''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "StopOnebyOne"
        self.startall = SimulStartLite(cm)
        self.stop = StopTest(cm)

    def __call__(self, dummy):
        '''Perform the 'StopOnebyOne' test. '''
        self.incr("calls")

        #     We ignore the "node" parameter...

        #     Start up all the nodes...
        ret = self.startall(None)
        if not ret:
            return self.failure("Setup failed")

        failed = []
        self.set_timer()
        for node in self.Env["nodes"]:
            if not self.stop(node):
                failed.append(node)

        if len(failed) > 0:
            return self.failure("Some node failed to stop: " + repr(failed))

        self.CM.clear_all_caches()
        return self.success()

#     Register StopOnebyOne as a good test to run
AllTestClasses.append(StopOnebyOne)


class RestartOnebyOne(CTSTest):
    '''Restart all the nodes in order'''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "RestartOnebyOne"
        self.startall = SimulStartLite(cm)

    def __call__(self, dummy):
        '''Perform the 'RestartOnebyOne' test. '''
        self.incr("calls")

        #     We ignore the "node" parameter...

        #     Start up all the nodes...
        ret = self.startall(None)
        if not ret:
            return self.failure("Setup failed")

        did_fail = []
        self.set_timer()
        self.restart = RestartTest(self.CM)
        for node in self.Env["nodes"]:
            if not self.restart(node):
                did_fail.append(node)

        if did_fail:
            return self.failure("Could not restart %d nodes: %s"
                                % (len(did_fail), repr(did_fail)))
        return self.success()

#     Register StopOnebyOne as a good test to run
AllTestClasses.append(RestartOnebyOne)


class PartialStart(CTSTest):
    '''Start a node - but tell it to stop before it finishes starting up'''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "PartialStart"
        self.startall = SimulStartLite(cm)
        self.stopall = SimulStopLite(cm)
        self.stop = StopTest(cm)
        #self.is_unsafe = 1

    def __call__(self, node):
        '''Perform the 'PartialStart' test. '''
        self.incr("calls")

        ret = self.stopall(None)
        if not ret:
            return self.failure("Setup failed")

#   FIXME!  This should use the CM class to get the pattern
#       then it would be applicable in general
        watchpats = []
        watchpats.append("crmd.*Connecting to cluster infrastructure")
        watch = self.create_watch(watchpats, self.Env["DeadTime"]+10)
        watch.setwatch()

        self.CM.StartaCMnoBlock(node)
        ret = watch.lookforall()
        if not ret:
            self.logger.log("Patterns not found: " + repr(watch.unmatched))
            return self.failure("Setup of %s failed" % node)

        ret = self.stop(node)
        if not ret:
            return self.failure("%s did not stop in time" % node)

        return self.success()

    def errorstoignore(self):
        '''Return list of errors which should be ignored'''

        # We might do some fencing in the 2-node case if we make it up far enough
        return [
            """Executing reboot fencing operation""",
        ]

#     Register StopOnebyOne as a good test to run
AllTestClasses.append(PartialStart)


class StandbyTest(CTSTest):
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "Standby"
        self.benchmark = 1

        self.start = StartTest(cm)
        self.startall = SimulStartLite(cm)

    # make sure the node is active
    # set the node to standby mode
    # check resources, none resource should be running on the node
    # set the node to active mode
    # check resouces, resources should have been migrated back (SHOULD THEY?)

    def __call__(self, node):

        self.incr("calls")
        ret = self.startall(None)
        if not ret:
            return self.failure("Start all nodes failed")

        self.debug("Make sure node %s is active" % node)
        if self.CM.StandbyStatus(node) != "off":
            if not self.CM.SetStandbyMode(node, "off"):
                return self.failure("can't set node %s to active mode" % node)

        self.CM.cluster_stable()

        status = self.CM.StandbyStatus(node)
        if status != "off":
            return self.failure("standby status of %s is [%s] but we expect [off]" % (node, status))

        self.debug("Getting resources running on node %s" % node)
        rsc_on_node = self.CM.active_resources(node)

        watchpats = []
        watchpats.append(r"State transition .* -> S_POLICY_ENGINE")
        watch = self.create_watch(watchpats, self.Env["DeadTime"]+10)
        watch.setwatch()

        self.debug("Setting node %s to standby mode" % node)
        if not self.CM.SetStandbyMode(node, "on"):
            return self.failure("can't set node %s to standby mode" % node)

        self.set_timer("on")

        ret = watch.lookforall()
        if not ret:
            self.logger.log("Patterns not found: " + repr(watch.unmatched))
            self.CM.SetStandbyMode(node, "off")
            return self.failure("cluster didn't react to standby change on %s" % node)

        self.CM.cluster_stable()

        status = self.CM.StandbyStatus(node)
        if status != "on":
            return self.failure("standby status of %s is [%s] but we expect [on]" % (node, status))
        self.log_timer("on")

        self.debug("Checking resources")
        bad_run = self.CM.active_resources(node)
        if len(bad_run) > 0:
            rc = self.failure("%s set to standby, %s is still running on it" % (node, repr(bad_run)))
            self.debug("Setting node %s to active mode" % node)
            self.CM.SetStandbyMode(node, "off")
            return rc

        self.debug("Setting node %s to active mode" % node)
        if not self.CM.SetStandbyMode(node, "off"):
            return self.failure("can't set node %s to active mode" % node)

        self.set_timer("off")
        self.CM.cluster_stable()

        status = self.CM.StandbyStatus(node)
        if status != "off":
            return self.failure("standby status of %s is [%s] but we expect [off]" % (node, status))
        self.log_timer("off")

        return self.success()

AllTestClasses.append(StandbyTest)


class ValgrindTest(CTSTest):
    '''Check for memory leaks'''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "Valgrind"
        self.stopall = SimulStopLite(cm)
        self.startall = SimulStartLite(cm)
        self.is_valgrind = 1
        self.is_loop = 1

    def setup(self, node):
        self.incr("calls")

        ret = self.stopall(None)
        if not ret:
            return self.failure("Stop all nodes failed")

        # Enable valgrind
        self.logger.logPat = "/tmp/%s-*.valgrind" % self.name

        self.Env["valgrind-prefix"] = self.name

        self.rsh(node, "rm -f %s" % self.logger.logPat, None)

        ret = self.startall(None)
        if not ret:
            return self.failure("Start all nodes failed")

        for node in self.Env["nodes"]:
            (rc, output) = self.rsh(node, "ps u --ppid `pidofproc aisexec`", None)
            for line in output:
                self.debug(line)

        return self.success()

    def teardown(self, node):
        # Disable valgrind
        self.Env["valgrind-prefix"] = None

        # Return all nodes to normal
        ret = self.stopall(None)
        if not ret:
            return self.failure("Stop all nodes failed")

        return self.success()

    def find_leaks(self):
        # Check for leaks
        leaked = []
        self.stop = StopTest(self.CM)

        for node in self.Env["nodes"]:
            (rc, ps_out) = self.rsh(node, "ps u --ppid `pidofproc aisexec`", None)
            rc = self.stop(node)
            if not rc:
                self.failure("Couldn't shut down %s" % node)

            rc = self.rsh(node, "grep -e indirectly.*lost:.*[1-9] -e definitely.*lost:.*[1-9] -e (ERROR|error).*SUMMARY:.*[1-9].*errors %s" % self.logger.logPat, 0)
            if rc != 1:
                leaked.append(node)
                self.failure("Valgrind errors detected on %s" % node)
                for line in ps_out:
                    self.logger.log(line)
                (rc, output) = self.rsh(node, "grep -e lost: -e SUMMARY: %s" % self.logger.logPat, None)
                for line in output:
                    self.logger.log(line)
                (rc, output) = self.rsh(node, "cat %s" % self.logger.logPat, None)
                for line in output:
                    self.debug(line)

        self.rsh(node, "rm -f %s" % self.logger.logPat, None)
        return leaked

    def __call__(self, node):
        leaked = self.find_leaks()
        if len(leaked) > 0:
            return self.failure("Nodes %s leaked" % repr(leaked))

        return self.success()

    def errorstoignore(self):
        '''Return list of errors which should be ignored'''
        return [
            r"cib.*: \*\*\*\*\*\*\*\*\*\*\*\*\*",
            r"cib.*: .* avoid confusing Valgrind",
            r"HA_VALGRIND_ENABLED",
        ]


class StandbyLoopTest(ValgrindTest):
    '''Check for memory leaks by putting a node in and out of standby for an hour'''
    def __init__(self, cm):
        ValgrindTest.__init__(self,cm)
        self.name = "StandbyLoop"

    def __call__(self, node):

        lpc = 0
        delay = 2
        failed = 0
        done = time.time() + self.Env["loop-minutes"] * 60
        while time.time() <= done and not failed:
            lpc = lpc + 1

            time.sleep(delay)
            if not self.CM.SetStandbyMode(node, "on"):
                self.failure("can't set node %s to standby mode" % node)
                failed = lpc

            time.sleep(delay)
            if not self.CM.SetStandbyMode(node, "off"):
                self.failure("can't set node %s to active mode" % node)
                failed = lpc

        leaked = self.find_leaks()
        if failed:
            return self.failure("Iteration %d failed" % failed)
        elif len(leaked) > 0:
            return self.failure("Nodes %s leaked" % repr(leaked))

        return self.success()

AllTestClasses.append(StandbyLoopTest)


class BandwidthTest(CTSTest):
#        Tests should not be cluster-manager-specific
#        If you need to find out cluster manager configuration to do this, then
#        it should be added to the generic cluster manager API.
    '''Test the bandwidth which heartbeat uses'''
    def __init__(self, cm):
        CTSTest.__init__(self, cm)
        self.name = "Bandwidth"
        self.start = StartTest(cm)
        self.__setitem__("min",0)
        self.__setitem__("max",0)
        self.__setitem__("totalbandwidth",0)
        (handle, self.tempfile) = tempfile.mkstemp(".cts")
        os.close(handle)
        self.startall = SimulStartLite(cm)

    def __call__(self, node):
        '''Perform the Bandwidth test'''
        self.incr("calls")

        if self.CM.upcount() < 1:
            return self.skipped()

        Path = self.CM.InternalCommConfig()
        if "ip" not in Path["mediatype"]:
             return self.skipped()

        port = Path["port"][0]
        port = int(port)

        ret = self.startall(None)
        if not ret:
            return self.failure("Test setup failed")
        time.sleep(5)  # We get extra messages right after startup.

        fstmpfile = "/var/run/band_estimate"
        dumpcmd = "tcpdump -p -n -c 102 -i any udp port %d > %s 2>&1" \
        %                (port, fstmpfile)

        rc = self.rsh(node, dumpcmd)
        if rc == 0:
            farfile = "root@%s:%s" % (node, fstmpfile)
            self.rsh.cp(farfile, self.tempfile)
            Bandwidth = self.countbandwidth(self.tempfile)
            if not Bandwidth:
                self.logger.log("Could not compute bandwidth.")
                return self.success()
            intband = int(Bandwidth + 0.5)
            self.logger.log("...bandwidth: %d bits/sec" % intband)
            self.Stats["totalbandwidth"] = self.Stats["totalbandwidth"] + Bandwidth
            if self.Stats["min"] == 0:
                self.Stats["min"] = Bandwidth
            if Bandwidth > self.Stats["max"]:
                self.Stats["max"] = Bandwidth
            if Bandwidth < self.Stats["min"]:
                self.Stats["min"] = Bandwidth
            self.rsh(node, "rm -f %s" % fstmpfile)
            os.unlink(self.tempfile)
            return self.success()
        else:
            return self.failure("no response from tcpdump command [%d]!" % rc)

    def countbandwidth(self, file):
        fp = open(file, "r")
        fp.seek(0)
        count = 0
        sum = 0
        while 1:
            line = fp.readline()
            if not line:
                return None
            if re.search("udp",line) or re.search("UDP,", line):
                count = count + 1
                linesplit = string.split(line," ")
                for j in range(len(linesplit)-1):
                    if linesplit[j] == "udp": break
                    if linesplit[j] == "length:": break

                try:
                    sum = sum + int(linesplit[j+1])
                except ValueError:
                    self.logger.log("Invalid tcpdump line: %s" % line)
                    return None
                T1 = linesplit[0]
                timesplit = string.split(T1,":")
                time2split = string.split(timesplit[2],".")
                time1 = (int(timesplit[0])*60+int(timesplit[1]))*60+int(time2split[0])+int(time2split[1])*0.000001
                break

        while count < 100:
            line = fp.readline()
            if not line:
                return None
            if re.search("udp",line) or re.search("UDP,", line):
                count = count+1
                linessplit = string.split(line," ")
                for j in range(len(linessplit)-1):
                    if linessplit[j] == "udp": break
                    if linesplit[j] == "length:": break
                try:
                    sum = int(linessplit[j+1]) + sum
                except ValueError:
                    self.logger.log("Invalid tcpdump line: %s" % line)
                    return None

        T2 = linessplit[0]
        timesplit = string.split(T2,":")
        time2split = string.split(timesplit[2],".")
        time2 = (int(timesplit[0])*60+int(timesplit[1]))*60+int(time2split[0])+int(time2split[1])*0.000001
        time = time2-time1
        if (time <= 0):
            return 0
        return (sum*8)/time

    def is_applicable(self):
        '''BandwidthTest never applicable'''
        return 0

AllTestClasses.append(BandwidthTest)


###################################################################
class MaintenanceMode(CTSTest):
###################################################################
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "MaintenanceMode"
        self.start = StartTest(cm)
        self.startall = SimulStartLite(cm)
        self.max = 30
        #self.is_unsafe = 1
        self.benchmark = 1
        self.action = "asyncmon"
        self.interval = 0
        self.rid = "maintenanceDummy"

    def toggleMaintenanceMode(self, node, action):
        pats = []
        pats.append(self.templates["Pat:DC_IDLE"])

        # fail the resource right after turning Maintenance mode on
        # verify it is not recovered until maintenance mode is turned off
        if action == "On":
            pats.append(r"pengine.*:\s+warning:.*Processing failed op %s for %s on" % (self.action, self.rid))
        else:
            pats.append(self.templates["Pat:RscOpOK"] % (self.rid, "stop_0"))
            pats.append(self.templates["Pat:RscOpOK"] % (self.rid, "start_0"))

        watch = self.create_watch(pats, 60)
        watch.setwatch()

        self.debug("Turning maintenance mode %s" % action)
        self.rsh(node, self.templates["MaintenanceMode%s" % (action)])
        if (action == "On"):
            self.rsh(node, "crm_resource -V -F -r %s -H %s &>/dev/null" % (self.rid, node))

        self.set_timer("recover%s" % (action))
        watch.lookforall()
        self.log_timer("recover%s" % (action))
        if watch.unmatched:
            self.debug("Failed to find patterns when turning maintenance mode %s" % action)
            return repr(watch.unmatched)

        return ""

    def insertMaintenanceDummy(self, node):
        pats = []
        pats.append(("%s.*" % node) + (self.templates["Pat:RscOpOK"] % (self.rid, "start_0")))

        watch = self.create_watch(pats, 60)
        watch.setwatch()

        self.CM.AddDummyRsc(node, self.rid)

        self.set_timer("addDummy")
        watch.lookforall()
        self.log_timer("addDummy")

        if watch.unmatched:
            self.debug("Failed to find patterns when adding maintenance dummy resource")
            return repr(watch.unmatched)
        return ""

    def removeMaintenanceDummy(self, node):
        pats = []
        pats.append(self.templates["Pat:RscOpOK"] % (self.rid, "stop_0"))

        watch = self.create_watch(pats, 60)
        watch.setwatch()
        self.CM.RemoveDummyRsc(node, self.rid)

        self.set_timer("removeDummy")
        watch.lookforall()
        self.log_timer("removeDummy")

        if watch.unmatched:
            self.debug("Failed to find patterns when removing maintenance dummy resource")
            return repr(watch.unmatched)
        return ""

    def managedRscList(self, node):
        rscList = []
        (rc, lines) = self.rsh(node, "crm_resource -c", None)
        for line in lines:
            if re.search("^Resource", line):
                tmp = AuditResource(self.CM, line)
                if tmp.managed():
                    rscList.append(tmp.id)

        return rscList

    def verifyResources(self, node, rscList, managed):
        managedList = list(rscList)
        managed_str = "managed"
        if not managed:
            managed_str = "unmanaged"

        (rc, lines) = self.rsh(node, "crm_resource -c", None)
        for line in lines:
            if re.search("^Resource", line):
                tmp = AuditResource(self.CM, line)
                if managed and not tmp.managed():
                    continue
                elif not managed and tmp.managed():
                    continue
                elif managedList.count(tmp.id):
                    managedList.remove(tmp.id)

        if len(managedList) == 0:
            self.debug("Found all %s resources on %s" % (managed_str, node))
            return True

        self.logger.log("Could not find all %s resources on %s. %s" % (managed_str, node, managedList))
        return False

    def __call__(self, node):
        '''Perform the 'MaintenanceMode' test. '''
        self.incr("calls")
        verify_managed = False
        verify_unmanaged = False
        failPat = ""

        ret = self.startall(None)
        if not ret:
            return self.failure("Setup failed")

        # get a list of all the managed resources. We use this list
        # after enabling maintenance mode to verify all managed resources
        # become un-managed.  After maintenance mode is turned off, we use
        # this list to verify all the resources become managed again.
        managedResources = self.managedRscList(node)
        if len(managedResources) == 0:
            self.logger.log("No managed resources on %s" % node)
            return self.skipped()

        # insert a fake resource we can fail during maintenance mode
        # so we can verify recovery does not take place until after maintenance
        # mode is disabled.
        failPat = failPat + self.insertMaintenanceDummy(node)

        # toggle maintenance mode ON, then fail dummy resource.
        failPat = failPat + self.toggleMaintenanceMode(node, "On")

        # verify all the resources are now unmanaged
        if self.verifyResources(node, managedResources, False):
            verify_unmanaged = True

        # Toggle maintenance mode  OFF, verify dummy is recovered.
        failPat = failPat + self.toggleMaintenanceMode(node, "Off")

        # verify all the resources are now managed again
        if self.verifyResources(node, managedResources, True):
            verify_managed = True

        # Remove our maintenance dummy resource.
        failPat = failPat + self.removeMaintenanceDummy(node)

        self.CM.cluster_stable()

        if failPat != "":
            return self.failure("Unmatched patterns: %s" % (failPat))
        elif verify_unmanaged is False:
            return self.failure("Failed to verify resources became unmanaged during maintenance mode")
        elif verify_managed is False:
            return self.failure("Failed to verify resources switched back to managed after disabling maintenance mode")

        return self.success()

    def errorstoignore(self):
        '''Return list of errors which should be ignored'''
        return [
            r"Updating failcount for %s" % self.rid,
            r"pengine.*: Recover %s\s*\(.*\)" % self.rid,
            r"Unknown operation: fail",
            r"(ERROR|error): sending stonithRA op to stonithd failed.",
            self.templates["Pat:RscOpOK"] % (self.rid, ("%s_%d" % (self.action, self.interval))),
            r"(ERROR|error).*: Action %s_%s_%d .* initiated outside of a transition" % (self.rid, self.action, self.interval),
        ]

AllTestClasses.append(MaintenanceMode)


class ResourceRecover(CTSTest):
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "ResourceRecover"
        self.start = StartTest(cm)
        self.startall = SimulStartLite(cm)
        self.max = 30
        self.rid = None
        self.rid_alt = None
        #self.is_unsafe = 1
        self.benchmark = 1

        # these are the values used for the new LRM API call
        self.action = "asyncmon"
        self.interval = 0

    def __call__(self, node):
        '''Perform the 'ResourceRecover' test. '''
        self.incr("calls")

        ret = self.startall(None)
        if not ret:
            return self.failure("Setup failed")

        resourcelist = self.CM.active_resources(node)
        # if there are no resourcelist, return directly
        if len(resourcelist) == 0:
            self.logger.log("No active resources on %s" % node)
            return self.skipped()

        self.rid = self.Env.RandomGen.choice(resourcelist)
        self.rid_alt = self.rid

        rsc = None
        (rc, lines) = self.rsh(node, "crm_resource -c", None)
        for line in lines:
            if re.search("^Resource", line):
                tmp = AuditResource(self.CM, line)
                if tmp.id == self.rid:
                    rsc = tmp
                    # Handle anonymous clones that get renamed
                    self.rid = rsc.clone_id
                    break

        if not rsc:
            return self.failure("Could not find %s in the resource list" % self.rid)

        self.debug("Shooting %s aka. %s" % (rsc.clone_id, rsc.id))

        pats = []
        pats.append(r"pengine.*:\s+warning:.*Processing failed op %s for (%s|%s) on" % (self.action,
            rsc.id, rsc.clone_id))

        if rsc.managed():
            pats.append(self.templates["Pat:RscOpOK"] % (self.rid, "stop_0"))
            if rsc.unique():
                pats.append(self.templates["Pat:RscOpOK"] % (self.rid, "start_0"))
            else:
                # Anonymous clones may get restarted with a different clone number
                pats.append(self.templates["Pat:RscOpOK"] % (".*", "start_0"))

        watch = self.create_watch(pats, 60)
        watch.setwatch()

        self.rsh(node, "crm_resource -V -F -r %s -H %s &>/dev/null" % (self.rid, node))

        self.set_timer("recover")
        watch.lookforall()
        self.log_timer("recover")

        self.CM.cluster_stable()
        recovered = self.CM.ResourceLocation(self.rid)

        if watch.unmatched:
            return self.failure("Patterns not found: %s" % repr(watch.unmatched))

        elif rsc.unique() and len(recovered) > 1:
            return self.failure("%s is now active on more than one node: %s"%(self.rid, repr(recovered)))

        elif len(recovered) > 0:
            self.debug("%s is running on: %s" % (self.rid, repr(recovered)))

        elif rsc.managed():
            return self.failure("%s was not recovered and is inactive" % self.rid)

        return self.success()

    def errorstoignore(self):
        '''Return list of errors which should be ignored'''
        return [
            r"Updating failcount for %s" % self.rid,
            r"pengine.*: Recover (%s|%s)\s*\(.*\)" % (self.rid, self.rid_alt),
            r"Unknown operation: fail",
            r"(ERROR|error): sending stonithRA op to stonithd failed.",
            self.templates["Pat:RscOpOK"] % (self.rid, ("%s_%d" % (self.action, self.interval))),
            r"(ERROR|error).*: Action %s_%s_%d .* initiated outside of a transition" % (self.rid, self.action, self.interval),
        ]

AllTestClasses.append(ResourceRecover)


class ComponentFail(CTSTest):
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "ComponentFail"
        # TODO make this work correctly in docker.
        self.is_docker_unsafe = 1
        self.startall = SimulStartLite(cm)
        self.complist = cm.Components()
        self.patterns = []
        self.okerrpatterns = []
        self.is_unsafe = 1

    def __call__(self, node):
        '''Perform the 'ComponentFail' test. '''
        self.incr("calls")
        self.patterns = []
        self.okerrpatterns = []

        # start all nodes
        ret = self.startall(None)
        if not ret:
            return self.failure("Setup failed")

        if not self.CM.cluster_stable(self.Env["StableTime"]):
            return self.failure("Setup failed - unstable")

        node_is_dc = self.CM.is_node_dc(node, None)

        # select a component to kill
        chosen = self.Env.RandomGen.choice(self.complist)
        while chosen.dc_only == 1 and node_is_dc == 0:
            chosen = self.Env.RandomGen.choice(self.complist)

        self.debug("...component %s (dc=%d,boot=%d)" % (chosen.name, node_is_dc,chosen.triggersreboot))
        self.incr(chosen.name)

        if chosen.name != "aisexec" and chosen.name != "corosync":
            if self.Env["Name"] != "crm-lha" or chosen.name != "pengine":
                self.patterns.append(self.templates["Pat:ChildKilled"] %(node, chosen.name))
                self.patterns.append(self.templates["Pat:ChildRespawn"] %(node, chosen.name))

        self.patterns.extend(chosen.pats)
        if node_is_dc:
          self.patterns.extend(chosen.dc_pats)

        # In an ideal world, this next stuff should be in the "chosen" object as a member function
        if self.Env["Name"] == "crm-lha" and chosen.triggersreboot:
            # Make sure the node goes down and then comes back up if it should reboot...
            for other in self.Env["nodes"]:
                if other != node:
                    self.patterns.append(self.templates["Pat:They_stopped"] %(other, self.CM.key_for_node(node)))
            self.patterns.append(self.templates["Pat:Slave_started"] % node)
            self.patterns.append(self.templates["Pat:Local_started"] % node)

            if chosen.dc_only:
                # Sometimes these will be in the log, and sometimes they won't...
                self.okerrpatterns.append("%s .*Process %s:.* exited" % (node, chosen.name))
                self.okerrpatterns.append("%s .*I_ERROR.*crmdManagedChildDied" % node)
                self.okerrpatterns.append("%s .*The %s subsystem terminated unexpectedly" % (node, chosen.name))
                self.okerrpatterns.append("(ERROR|error): Client .* exited with return code")
            else:
                # Sometimes this won't be in the log...
                self.okerrpatterns.append(self.templates["Pat:ChildKilled"] %(node, chosen.name))
                self.okerrpatterns.append(self.templates["Pat:ChildRespawn"] %(node, chosen.name))
                self.okerrpatterns.append(self.templates["Pat:ChildExit"])

        if chosen.name == "stonith":
            # Ignore actions for STONITH resources
            (rc, lines) = self.rsh(node, "crm_resource -c", None)
            for line in lines:
                if re.search("^Resource", line):
                    r = AuditResource(self.CM, line)
                    if r.rclass == "stonith":
                        self.okerrpatterns.append(self.templates["Pat:Fencing_recover"] % r.id)

        # supply a copy so self.patterns doesn't end up empty
        tmpPats = []
        tmpPats.extend(self.patterns)
        self.patterns.extend(chosen.badnews_ignore)

        # Look for STONITH ops, depending on Env["at-boot"] we might need to change the nodes status
        stonithPats = []
        stonithPats.append(self.templates["Pat:Fencing_ok"] % node)
        stonith = self.create_watch(stonithPats, 0)
        stonith.setwatch()

        # set the watch for stable
        watch = self.create_watch(
            tmpPats, self.Env["DeadTime"] + self.Env["StableTime"] + self.Env["StartTime"])
        watch.setwatch()

        # kill the component
        chosen.kill(node)

        self.debug("Waiting for the cluster to recover")
        self.CM.cluster_stable()

        self.debug("Waiting for any STONITHd node to come back up")
        self.CM.ns.WaitForAllNodesToComeUp(self.Env["nodes"], 600)

        self.debug("Waiting for the cluster to re-stabilize with all nodes")
        self.CM.cluster_stable(self.Env["StartTime"])

        self.debug("Checking if %s was shot" % node)
        shot = stonith.look(60)
        if shot:
            self.debug("Found: " + repr(shot))
            self.okerrpatterns.append(self.templates["Pat:Fencing_start"] % node)

            if self.Env["at-boot"] == 0:
                self.CM.ShouldBeStatus[node] = "down"

            # If fencing occurred, chances are many (if not all) the expected logs
            # will not be sent - or will be lost when the node reboots
            return self.success()

        # check for logs indicating a graceful recovery
        matched = watch.lookforall(allow_multiple_matches=1)
        if watch.unmatched:
            self.logger.log("Patterns not found: " + repr(watch.unmatched))

        self.debug("Waiting for the cluster to re-stabilize with all nodes")
        is_stable = self.CM.cluster_stable(self.Env["StartTime"])

        if not matched:
            return self.failure("Didn't find all expected %s patterns" % chosen.name)
        elif not is_stable:
            return self.failure("Cluster did not become stable after killing %s" % chosen.name)

        return self.success()

    def errorstoignore(self):
        '''Return list of errors which should be ignored'''
    # Note that okerrpatterns refers to the last time we ran this test
    # The good news is that this works fine for us...
        self.okerrpatterns.extend(self.patterns)
        return self.okerrpatterns

AllTestClasses.append(ComponentFail)


class SplitBrainTest(CTSTest):
    '''It is used to test split-brain. when the path between the two nodes break
       check the two nodes both take over the resource'''
    def __init__(self,cm):
        CTSTest.__init__(self,cm)
        self.name = "SplitBrain"
        self.start = StartTest(cm)
        self.startall = SimulStartLite(cm)
        self.is_experimental = 1

    def isolate_partition(self, partition):
        other_nodes = []
        other_nodes.extend(self.Env["nodes"])

        for node in partition:
            try:
                other_nodes.remove(node)
            except ValueError:
                self.logger.log("Node "+node+" not in " + repr(self.Env["nodes"]) + " from " +repr(partition))

        if len(other_nodes) == 0:
            return 1

        self.debug("Creating partition: " + repr(partition))
        self.debug("Everyone else: " + repr(other_nodes))

        for node in partition:
            if not self.CM.isolate_node(node, other_nodes):
                self.logger.log("Could not isolate %s" % node)
                return 0

        return 1

    def heal_partition(self, partition):
        other_nodes = []
        other_nodes.extend(self.Env["nodes"])

        for node in partition:
            try:
                other_nodes.remove(node)
            except ValueError:
                self.logger.log("Node "+node+" not in " + repr(self.Env["nodes"]))

        if len(other_nodes) == 0:
            return 1

        self.debug("Healing partition: " + repr(partition))
        self.debug("Everyone else: " + repr(other_nodes))

        for node in partition:
            self.CM.unisolate_node(node, other_nodes)

    def __call__(self, node):
        '''Perform split-brain test'''
        self.incr("calls")
        self.passed = 1
        partitions = {}

        ret = self.startall(None)
        if not ret:
            return self.failure("Setup failed")

        while 1:
            # Retry until we get multiple partitions
            partitions = {}
            p_max = len(self.Env["nodes"])
            for node in self.Env["nodes"]:
                p = self.Env.RandomGen.randint(1, p_max)
                if not p in partitions:
                    partitions[p] = []
                partitions[p].append(node)
            p_max = len(partitions.keys())
            if p_max > 1:
                break
            # else, try again

        self.debug("Created %d partitions" % p_max)
        for key in list(partitions.keys()):
            self.debug("Partition["+str(key)+"]:\t"+repr(partitions[key]))

        # Disabling STONITH to reduce test complexity for now
        self.rsh(node, "crm_attribute -V -n stonith-enabled -v false")

        for key in list(partitions.keys()):
            self.isolate_partition(partitions[key])

        count = 30
        while count > 0:
            if len(self.CM.find_partitions()) != p_max:
                time.sleep(10)
            else:
                break
        else:
            self.failure("Expected partitions were not created")

        # Target number of partitions formed - wait for stability
        if not self.CM.cluster_stable():
            self.failure("Partitioned cluster not stable")

        # Now audit the cluster state
        self.CM.partitions_expected = p_max
        if not self.audit():
            self.failure("Audits failed")
        self.CM.partitions_expected = 1

        # And heal them again
        for key in list(partitions.keys()):
            self.heal_partition(partitions[key])

        # Wait for a single partition to form
        count = 30
        while count > 0:
            if len(self.CM.find_partitions()) != 1:
                time.sleep(10)
                count -= 1
            else:
                break
        else:
            self.failure("Cluster did not reform")

        # Wait for it to have the right number of members
        count = 30
        while count > 0:
            members = []

            partitions = self.CM.find_partitions()
            if len(partitions) > 0:
                members = partitions[0].split()

            if len(members) != len(self.Env["nodes"]):
                time.sleep(10)
                count -= 1
            else:
                break
        else:
            self.failure("Cluster did not completely reform")

        # Wait up to 20 minutes - the delay is more preferable than
        # trying to continue with in a messed up state
        if not self.CM.cluster_stable(1200):
            self.failure("Reformed cluster not stable")
            if self.Env["continue"] == 1:
                answer = "Y"
            else:
                try:
                    answer = raw_input('Continue? [nY]')
                except EOFError, e:
                    answer = "n" 
            if answer and answer == "n":
                raise ValueError("Reformed cluster not stable")

        # Turn fencing back on
        if self.Env["DoFencing"]:
            self.rsh(node, "crm_attribute -V -D -n stonith-enabled")

        self.CM.cluster_stable()

        if self.passed:
            return self.success()
        return self.failure("See previous errors")

    def errorstoignore(self):
        '''Return list of errors which are 'normal' and should be ignored'''
        return [
            r"Another DC detected:",
            r"(ERROR|error).*: .*Application of an update diff failed",
            r"crmd.*:.*not in our membership list",
            r"CRIT:.*node.*returning after partition",
        ]

    def is_applicable(self):
        if not self.is_applicable_common():
            return 0
        return len(self.Env["nodes"]) > 2

AllTestClasses.append(SplitBrainTest)


class Reattach(CTSTest):
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "Reattach"
        self.startall = SimulStartLite(cm)
        self.restart1 = RestartTest(cm)
        self.stopall = SimulStopLite(cm)
        self.is_unsafe = 0 # Handled by canrunnow()

    def _is_managed(self, node):
        is_managed = self.rsh(node, "crm_attribute -t rsc_defaults -n is-managed -Q -G -d true", 1)
        is_managed = is_managed[:-1] # Strip off the newline
        return is_managed == "true"

    def _set_unmanaged(self, node):
        self.debug("Disable resource management")
        self.rsh(node, "crm_attribute -t rsc_defaults -n is-managed -v false")

    def _set_managed(self, node):
        self.debug("Re-enable resource management")
        self.rsh(node, "crm_attribute -t rsc_defaults -n is-managed -D")

    def setup(self, node):
        attempt = 0
        if not self.startall(None):
            return None

        # Make sure we are really _really_ stable and that all
        # resources, including those that depend on transient node
        # attributes, are started
        while not self.CM.cluster_stable(double_check=True):
            if attempt < 5:
                attempt += 1
                self.debug("Not stable yet, re-testing")
            else:
                self.logger.log("Cluster is not stable")
                return None

        return 1

    def teardown(self, node):

        # Make sure 'node' is up
        start = StartTest(self.CM)
        start(node)

        if not self._is_managed(node):
            self.logger.log("Attempting to re-enable resource management on %s" % node)
            self._set_managed(node)
            self.CM.cluster_stable()
            if not self._is_managed(node):
                self.logger.log("Could not re-enable resource management")
                return 0

        return 1

    def canrunnow(self, node):
        '''Return TRUE if we can meaningfully run right now'''
        if self.find_ocfs2_resources(node):
            self.logger.log("Detach/Reattach scenarios are not possible with OCFS2 services present")
            return 0
        return 1

    def __call__(self, node):
        self.incr("calls")

        pats = []
        # Conveniently, pengine will display this message when disabling management,
        # even if fencing is not enabled, so we can rely on it.
        managed = self.create_watch(["Delaying fencing operations"], 60)
        managed.setwatch()

        self._set_unmanaged(node)

        if not managed.lookforall():
            self.logger.log("Patterns not found: " + repr(managed.unmatched))
            return self.failure("Resource management not disabled")

        pats = []
        pats.append(self.templates["Pat:RscOpOK"] % (".*", "start"))
        pats.append(self.templates["Pat:RscOpOK"] % (".*", "stop"))
        pats.append(self.templates["Pat:RscOpOK"] % (".*", "promote"))
        pats.append(self.templates["Pat:RscOpOK"] % (".*", "demote"))
        pats.append(self.templates["Pat:RscOpOK"] % (".*", "migrate"))

        watch = self.create_watch(pats, 60, "ShutdownActivity")
        watch.setwatch()

        self.debug("Shutting down the cluster")
        ret = self.stopall(None)
        if not ret:
            self._set_managed(node)
            return self.failure("Couldn't shut down the cluster")

        self.debug("Bringing the cluster back up")
        ret = self.startall(None)
        time.sleep(5) # allow ping to update the CIB
        if not ret:
            self._set_managed(node)
            return self.failure("Couldn't restart the cluster")

        if self.local_badnews("ResourceActivity:", watch):
            self._set_managed(node)
            return self.failure("Resources stopped or started during cluster restart")

        watch = self.create_watch(pats, 60, "StartupActivity")
        watch.setwatch()

        # Re-enable resource management (and verify it happened).
        self._set_managed(node)
        self.CM.cluster_stable()
        if not self._is_managed(node):
            return self.failure("Could not re-enable resource management")

        # Ignore actions for STONITH resources
        ignore = []
        (rc, lines) = self.rsh(node, "crm_resource -c", None)
        for line in lines:
            if re.search("^Resource", line):
                r = AuditResource(self.CM, line)
                if r.rclass == "stonith":

                    self.debug("Ignoring start actions for %s" % r.id)
                    ignore.append(self.templates["Pat:RscOpOK"] % (r.id, "start_0"))

        if self.local_badnews("ResourceActivity:", watch, ignore):
            return self.failure("Resources stopped or started after resource management was re-enabled")

        return ret

    def errorstoignore(self):
        '''Return list of errors which should be ignored'''
        return [
            r"resources were active at shutdown",
        ]

    def is_applicable(self):
        if self.Env["Name"] == "crm-lha":
            return None
        return 1

AllTestClasses.append(Reattach)


class SpecialTest1(CTSTest):
    '''Set up a custom test to cause quorum failure issues for Andrew'''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "SpecialTest1"
        self.startall = SimulStartLite(cm)
        self.restart1 = RestartTest(cm)
        self.stopall = SimulStopLite(cm)

    def __call__(self, node):
        '''Perform the 'SpecialTest1' test for Andrew. '''
        self.incr("calls")

        #        Shut down all the nodes...
        ret = self.stopall(None)
        if not ret:
            return self.failure("Could not stop all nodes")

        # Test config recovery when the other nodes come up
        self.rsh(node, "rm -f "+CTSvars.CRM_CONFIG_DIR+"/cib*")

        #        Start the selected node
        ret = self.restart1(node)
        if not ret:
            return self.failure("Could not start "+node)

        #        Start all remaining nodes
        ret = self.startall(None)
        if not ret:
            return self.failure("Could not start the remaining nodes")

        return self.success()

    def errorstoignore(self):
        '''Return list of errors which should be ignored'''
        # Errors that occur as a result of the CIB being wiped
        return [
            r"error.*: v1 patchset error, patch failed to apply: Application of an update diff failed",
            r"error.*: Resource start-up disabled since no STONITH resources have been defined",
            r"error.*: Either configure some or disable STONITH with the stonith-enabled option",
            r"error.*: NOTE: Clusters with shared data need STONITH to ensure data integrity",
        ]

AllTestClasses.append(SpecialTest1)


class HAETest(CTSTest):
    '''Set up a custom test to cause quorum failure issues for Andrew'''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "HAETest"
        self.stopall = SimulStopLite(cm)
        self.startall = SimulStartLite(cm)
        self.is_loop = 1

    def setup(self, node):
        #  Start all remaining nodes
        ret = self.startall(None)
        if not ret:
            return self.failure("Couldn't start all nodes")
        return self.success()

    def teardown(self, node):
        # Stop everything
        ret = self.stopall(None)
        if not ret:
            return self.failure("Couldn't stop all nodes")
        return self.success()

    def wait_on_state(self, node, resource, expected_clones, attempts=240):
        while attempts > 0:
            active = 0
            (rc, lines) = self.rsh(node, "crm_resource -r %s -W -Q" % resource, stdout=None)

            # Hack until crm_resource does the right thing
            if rc == 0 and lines:
                active = len(lines)

            if len(lines) == expected_clones:
                return 1

            elif rc == 1:
                self.debug("Resource %s is still inactive" % resource)

            elif rc == 234:
                self.logger.log("Unknown resource %s" % resource)
                return 0

            elif rc == 246:
                self.logger.log("Cluster is inactive")
                return 0

            elif rc != 0:
                self.logger.log("Call to crm_resource failed, rc=%d" % rc)
                return 0

            else:
                self.debug("Resource %s is active on %d times instead of %d" % (resource, active, expected_clones))

            attempts -= 1
            time.sleep(1)

        return 0

    def find_dlm(self, node):
        self.r_dlm = None

        (rc, lines) = self.rsh(node, "crm_resource -c", None)
        for line in lines:
            if re.search("^Resource", line):
                r = AuditResource(self.CM, line)
                if r.rtype == "controld" and r.parent != "NA":
                    self.debug("Found dlm: %s" % self.r_dlm)
                    self.r_dlm = r.parent
                    return 1
        return 0

    def find_hae_resources(self, node):
        self.r_dlm = None
        self.r_o2cb = None
        self.r_ocfs2 = []

        if self.find_dlm(node):
            self.find_ocfs2_resources(node)

    def is_applicable(self):
        if not self.is_applicable_common():
            return 0
        if self.Env["Schema"] == "hae":
            return 1
        return None


class HAERoleTest(HAETest):
    def __init__(self, cm):
        '''Lars' mount/unmount test for the HA extension. '''
        HAETest.__init__(self,cm)
        self.name = "HAERoleTest"

    def change_state(self, node, resource, target):
        rc = self.rsh(node, "crm_resource -V -r %s -p target-role -v %s  --meta" % (resource, target))
        return rc

    def __call__(self, node):
        self.incr("calls")
        lpc = 0
        failed = 0
        delay = 2
        done = time.time() + self.Env["loop-minutes"]*60
        self.find_hae_resources(node)

        clone_max = len(self.Env["nodes"])
        while time.time() <= done and not failed:
            lpc = lpc + 1

            self.change_state(node, self.r_dlm, "Stopped")
            if not self.wait_on_state(node, self.r_dlm, 0):
                self.failure("%s did not go down correctly" % self.r_dlm)
                failed = lpc

            self.change_state(node, self.r_dlm, "Started")
            if not self.wait_on_state(node, self.r_dlm, clone_max):
                self.failure("%s did not come up correctly" % self.r_dlm)
                failed = lpc

            if not self.wait_on_state(node, self.r_o2cb, clone_max):
                self.failure("%s did not come up correctly" % self.r_o2cb)
                failed = lpc

            for fs in self.r_ocfs2:
                if not self.wait_on_state(node, fs, clone_max):
                    self.failure("%s did not come up correctly" % fs)
                    failed = lpc

        if failed:
            return self.failure("iteration %d failed" % failed)
        return self.success()

AllTestClasses.append(HAERoleTest)


class HAEStandbyTest(HAETest):
    '''Set up a custom test to cause quorum failure issues for Andrew'''
    def __init__(self, cm):
        HAETest.__init__(self,cm)
        self.name = "HAEStandbyTest"

    def change_state(self, node, resource, target):
        rc = self.rsh(node, "crm_standby -V -l reboot -v %s" % (target))
        return rc

    def __call__(self, node):
        self.incr("calls")

        lpc = 0
        failed = 0
        done = time.time() + self.Env["loop-minutes"]*60
        self.find_hae_resources(node)

        clone_max = len(self.Env["nodes"])
        while time.time() <= done and not failed:
            lpc = lpc + 1

            self.change_state(node, self.r_dlm, "true")
            if not self.wait_on_state(node, self.r_dlm, clone_max-1):
                self.failure("%s did not go down correctly" % self.r_dlm)
                failed = lpc

            self.change_state(node, self.r_dlm, "false")
            if not self.wait_on_state(node, self.r_dlm, clone_max):
                self.failure("%s did not come up correctly" % self.r_dlm)
                failed = lpc

            if not self.wait_on_state(node, self.r_o2cb, clone_max):
                self.failure("%s did not come up correctly" % self.r_o2cb)
                failed = lpc

            for fs in self.r_ocfs2:
                if not self.wait_on_state(node, fs, clone_max):
                    self.failure("%s did not come up correctly" % fs)
                    failed = lpc

        if failed:
            return self.failure("iteration %d failed" % failed)
        return self.success()

AllTestClasses.append(HAEStandbyTest)


class NearQuorumPointTest(CTSTest):
    '''
    This test brings larger clusters near the quorum point (50%).
    In addition, it will test doing starts and stops at the same time.

    Here is how I think it should work:
    - loop over the nodes and decide randomly which will be up and which
      will be down  Use a 50% probability for each of up/down.
    - figure out what to do to get into that state from the current state
    - in parallel, bring up those going up  and bring those going down.
    '''

    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "NearQuorumPoint"

    def __call__(self, dummy):
        '''Perform the 'NearQuorumPoint' test. '''
        self.incr("calls")
        startset = []
        stopset = []

        stonith = self.CM.prepare_fencing_watcher("NearQuorumPoint")
        #decide what to do with each node
        for node in self.Env["nodes"]:
            action = self.Env.RandomGen.choice(["start","stop"])
            #action = self.Env.RandomGen.choice(["start","stop","no change"])
            if action == "start" :
                startset.append(node)
            elif action == "stop" :
                stopset.append(node)

        self.debug("start nodes:" + repr(startset))
        self.debug("stop nodes:" + repr(stopset))

        #add search patterns
        watchpats = [ ]
        for node in stopset:
            if self.CM.ShouldBeStatus[node] == "up":
                watchpats.append(self.templates["Pat:We_stopped"] % node)

        for node in startset:
            if self.CM.ShouldBeStatus[node] == "down":
                #watchpats.append(self.templates["Pat:Slave_started"] % node)
                watchpats.append(self.templates["Pat:Local_started"] % node)
            else:
                for stopping in stopset:
                    if self.CM.ShouldBeStatus[stopping] == "up":
                        watchpats.append(self.templates["Pat:They_stopped"] % (node, self.CM.key_for_node(stopping)))

        if len(watchpats) == 0:
            return self.skipped()

        if len(startset) != 0:
            watchpats.append(self.templates["Pat:DC_IDLE"])

        watch = self.create_watch(watchpats, self.Env["DeadTime"]+10)

        watch.setwatch()

        #begin actions
        for node in stopset:
            if self.CM.ShouldBeStatus[node] == "up":
                self.CM.StopaCMnoBlock(node)

        for node in startset:
            if self.CM.ShouldBeStatus[node] == "down":
                self.CM.StartaCMnoBlock(node)

        #get the result
        if watch.lookforall():
            self.CM.cluster_stable()
            self.CM.fencing_cleanup("NearQuorumPoint", stonith)
            return self.success()

        self.logger.log("Warn: Patterns not found: " + repr(watch.unmatched))

        #get the "bad" nodes
        upnodes = []
        for node in stopset:
            if self.CM.StataCM(node) == 1:
                upnodes.append(node)

        downnodes = []
        for node in startset:
            if self.CM.StataCM(node) == 0:
                downnodes.append(node)

        self.CM.fencing_cleanup("NearQuorumPoint", stonith)
        if upnodes == [] and downnodes == []:
            self.CM.cluster_stable()

            # Make sure they're completely down with no residule
            for node in stopset:
                self.rsh(node, self.templates["StopCmd"])

            return self.success()

        if len(upnodes) > 0:
            self.logger.log("Warn: Unstoppable nodes: " + repr(upnodes))

        if len(downnodes) > 0:
            self.logger.log("Warn: Unstartable nodes: " + repr(downnodes))

        return self.failure()

    def is_applicable(self):
        if self.Env["Name"] == "crm-cman":
            return None
        return 1

AllTestClasses.append(NearQuorumPointTest)


class RollingUpgradeTest(CTSTest):
    '''Perform a rolling upgrade of the cluster'''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "RollingUpgrade"
        self.start = StartTest(cm)
        self.stop = StopTest(cm)
        self.stopall = SimulStopLite(cm)
        self.startall = SimulStartLite(cm)

    def setup(self, node):
        #  Start all remaining nodes
        ret = self.stopall(None)
        if not ret:
            return self.failure("Couldn't stop all nodes")

        for node in self.Env["nodes"]:
            if not self.downgrade(node, None):
                return self.failure("Couldn't downgrade %s" % node)

        ret = self.startall(None)
        if not ret:
            return self.failure("Couldn't start all nodes")
        return self.success()

    def teardown(self, node):
        # Stop everything
        ret = self.stopall(None)
        if not ret:
            return self.failure("Couldn't stop all nodes")

        for node in self.Env["nodes"]:
            if not self.upgrade(node, None):
                return self.failure("Couldn't upgrade %s" % node)

        return self.success()

    def install(self, node, version, start=1, flags="--force"):

        target_dir = "/tmp/rpm-%s" % version
        src_dir = "%s/%s" % (self.Env["rpm-dir"], version)

        self.logger.log("Installing %s on %s with %s" % (version, node, flags))
        if not self.stop(node):
            return self.failure("stop failure: "+node)

        rc = self.rsh(node, "mkdir -p %s" % target_dir)
        rc = self.rsh(node, "rm -f %s/*.rpm" % target_dir)
        (rc, lines) = self.rsh(node, "ls -1 %s/*.rpm" % src_dir, None)
        for line in lines:
            line = line[:-1]
            rc = self.rsh.cp("%s" % (line), "%s:%s/" % (node, target_dir))
        rc = self.rsh(node, "rpm -Uvh %s %s/*.rpm" % (flags, target_dir))

        if start and not self.start(node):
            return self.failure("start failure: "+node)

        return self.success()

    def upgrade(self, node, start=1):
        return self.install(node, self.Env["current-version"], start)

    def downgrade(self, node, start=1):
        return self.install(node, self.Env["previous-version"], start, "--force --nodeps")

    def __call__(self, node):
        '''Perform the 'Rolling Upgrade' test. '''
        self.incr("calls")

        for node in self.Env["nodes"]:
            if self.upgrade(node):
                return self.failure("Couldn't upgrade %s" % node)

            self.CM.cluster_stable()

        return self.success()

    def is_applicable(self):
        if not self.is_applicable_common():
            return None

        if not "rpm-dir" in self.Env.keys():
            return None
        if not "current-version" in self.Env.keys():
            return None
        if not "previous-version" in self.Env.keys():
            return None

        return 1

#        Register RestartTest as a good test to run
AllTestClasses.append(RollingUpgradeTest)


class BSC_AddResource(CTSTest):
    '''Add a resource to the cluster'''
    def __init__(self, cm):
        CTSTest.__init__(self, cm)
        self.name = "AddResource"
        self.resource_offset = 0
        self.cib_cmd = """cibadmin -C -o %s -X '%s' """

    def __call__(self, node):
        self.incr("calls")
        self.resource_offset =         self.resource_offset  + 1

        r_id = "bsc-rsc-%s-%d" % (node, self.resource_offset)
        start_pat = "crmd.*%s_start_0.*confirmed.*ok"

        patterns = []
        patterns.append(start_pat % r_id)

        watch = self.create_watch(patterns, self.Env["DeadTime"])
        watch.setwatch()

        ip = self.NextIP()
        if not self.make_ip_resource(node, r_id, "ocf", "IPaddr", ip):
            return self.failure("Make resource %s failed" % r_id)

        failed = 0
        watch_result = watch.lookforall()
        if watch.unmatched:
            for regex in watch.unmatched:
                self.logger.log ("Warn: Pattern not found: %s" % (regex))
                failed = 1

        if failed:
            return self.failure("Resource pattern(s) not found")

        if not self.CM.cluster_stable(self.Env["DeadTime"]):
            return self.failure("Unstable cluster")

        return self.success()

    def NextIP(self):
        ip = self.Env["IPBase"]
        if ":" in ip:
            fields = ip.rpartition(":")
            fields[2] = str(hex(int(fields[2], 16)+1))
            print(str(hex(int(f[2], 16)+1)))
        else:
            fields = ip.rpartition('.')
            fields[2] = str(int(fields[2])+1)

        ip = fields[0] + fields[1] + fields[3];
        self.Env["IPBase"] = ip
        return ip.strip()

    def make_ip_resource(self, node, id, rclass, type, ip):
        self.logger.log("Creating %s::%s:%s (%s) on %s" % (rclass,type,id,ip,node))
        rsc_xml="""
<primitive id="%s" class="%s" type="%s"  provider="heartbeat">
    <instance_attributes id="%s"><attributes>
        <nvpair id="%s" name="ip" value="%s"/>
    </attributes></instance_attributes>
</primitive>""" % (id, rclass, type, id, id, ip)

        node_constraint = """
      <rsc_location id="run_%s" rsc="%s">
        <rule id="pref_run_%s" score="100">
          <expression id="%s_loc_expr" attribute="#uname" operation="eq" value="%s"/>
        </rule>
      </rsc_location>""" % (id, id, id, id, node)

        rc = 0
        (rc, lines) = self.rsh(node, self.cib_cmd % ("constraints", node_constraint), None)
        if rc != 0:
            self.logger.log("Constraint creation failed: %d" % rc)
            return None

        (rc, lines) = self.rsh(node, self.cib_cmd % ("resources", rsc_xml), None)
        if rc != 0:
            self.logger.log("Resource creation failed: %d" % rc)
            return None

        return 1

    def is_applicable(self):
        if self.Env["DoBSC"]:
            return 1
        return None

AllTestClasses.append(BSC_AddResource)


class SimulStopLite(CTSTest):
    '''Stop any active nodes ~ simultaneously'''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "SimulStopLite"

    def __call__(self, dummy):
        '''Perform the 'SimulStopLite' setup work. '''
        self.incr("calls")

        self.debug("Setup: " + self.name)

        #     We ignore the "node" parameter...
        watchpats = [ ]

        for node in self.Env["nodes"]:
            if self.CM.ShouldBeStatus[node] == "up":
                self.incr("WasStarted")
                watchpats.append(self.templates["Pat:We_stopped"] % node)
                #if self.Env["use_logd"]:
                #    watchpats.append(self.templates["Pat:Logd_stopped"] % node)

        if len(watchpats) == 0:
            self.CM.clear_all_caches()
            return self.success()

        #     Stop all the nodes - at about the same time...
        watch = self.create_watch(watchpats, self.Env["DeadTime"]+10)

        watch.setwatch()
        self.set_timer()
        for node in self.Env["nodes"]:
            if self.CM.ShouldBeStatus[node] == "up":
                self.CM.StopaCMnoBlock(node)
        if watch.lookforall():
            self.CM.clear_all_caches()

            # Make sure they're completely down with no residule
            for node in self.Env["nodes"]:
                self.rsh(node, self.templates["StopCmd"])

            return self.success()

        did_fail = 0
        up_nodes = []
        for node in self.Env["nodes"]:
            if self.CM.StataCM(node) == 1:
                did_fail = 1
                up_nodes.append(node)

        if did_fail:
            return self.failure("Active nodes exist: " + repr(up_nodes))

        self.logger.log("Warn: All nodes stopped but CTS didnt detect: "
                    + repr(watch.unmatched))

        self.CM.clear_all_caches()
        return self.failure("Missing log message: "+repr(watch.unmatched))

    def is_applicable(self):
        '''SimulStopLite is a setup test and never applicable'''
        return 0


class SimulStartLite(CTSTest):
    '''Start any stopped nodes ~ simultaneously'''
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "SimulStartLite"

    def __call__(self, dummy):
        '''Perform the 'SimulStartList' setup work. '''
        self.incr("calls")
        self.debug("Setup: " + self.name)

        #        We ignore the "node" parameter...
        node_list = []
        for node in self.Env["nodes"]:
            if self.CM.ShouldBeStatus[node] == "down":
                self.incr("WasStopped")
                node_list.append(node)

        self.set_timer()
        while len(node_list) > 0:
            # Repeat until all nodes come up
            watchpats = [ ]

            uppat = self.templates["Pat:Slave_started"]
            if self.CM.upcount() == 0:
                uppat = self.templates["Pat:Local_started"]

            watchpats.append(self.templates["Pat:DC_IDLE"])
            for node in node_list:
                watchpats.append(uppat % node)
                watchpats.append(self.templates["Pat:InfraUp"] % node)
                watchpats.append(self.templates["Pat:PacemakerUp"] % node)

            #   Start all the nodes - at about the same time...
            watch = self.create_watch(watchpats, self.Env["DeadTime"]+10)
            watch.setwatch()

            stonith = self.CM.prepare_fencing_watcher(self.name)

            for node in node_list:
                self.CM.StartaCMnoBlock(node)

            watch.lookforall()

            node_list = self.CM.fencing_cleanup(self.name, stonith)

            if node_list == None:
                return self.failure("Cluster did not stabilize")

            # Remove node_list messages from watch.unmatched
            for node in node_list:
                self.logger.debug("Dealing with stonith operations for %s" % repr(node_list))
                if watch.unmatched:
                    try:
                        watch.unmatched.remove(uppat % node)
                    except:
                        self.debug("Already matched: %s" % (uppat % node))
                    try:                        
                        watch.unmatched.remove(self.templates["Pat:InfraUp"] % node)
                    except:
                        self.debug("Already matched: %s" % (self.templates["Pat:InfraUp"] % node))
                    try:
                        watch.unmatched.remove(self.templates["Pat:PacemakerUp"] % node)
                    except:
                        self.debug("Already matched: %s" % (self.templates["Pat:PacemakerUp"] % node))

            if watch.unmatched:
                for regex in watch.unmatched:
                    self.logger.log ("Warn: Startup pattern not found: %s" %(regex))

            if not self.CM.cluster_stable():
                return self.failure("Cluster did not stabilize")

        did_fail = 0
        unstable = []
        for node in self.Env["nodes"]:
            if self.CM.StataCM(node) == 0:
                did_fail = 1
                unstable.append(node)

        if did_fail:
            return self.failure("Unstarted nodes exist: " + repr(unstable))

        unstable = []
        for node in self.Env["nodes"]:
            if not self.CM.node_stable(node):
                did_fail = 1
                unstable.append(node)

        if did_fail:
            return self.failure("Unstable cluster nodes exist: " + repr(unstable))

        return self.success()

    def is_applicable(self):
        '''SimulStartLite is a setup test and never applicable'''
        return 0


def TestList(cm, audits):
    result = []
    for testclass in AllTestClasses:
        bound_test = testclass(cm)
        if bound_test.is_applicable():
            bound_test.Audits = audits
            result.append(bound_test)
    return result


class RemoteLXC(CTSTest):
    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = "RemoteLXC"
        self.start = StartTest(cm)
        self.startall = SimulStartLite(cm)
        self.num_containers = 2
        self.is_container = 1
        self.is_docker_unsafe = 1
        self.failed = 0
        self.fail_string = ""

    def start_lxc_simple(self, node):

        # restore any artifacts laying around from a previous test.
        self.rsh(node, "/usr/share/pacemaker/tests/cts/lxc_autogen.sh -R &>/dev/null")

        # generate the containers, put them in the config, add some resources to them
        pats = [ ]
        watch = self.create_watch(pats, 120)
        watch.setwatch()
        pats.append(self.templates["Pat:RscOpOK"] % ("lxc1", "start_0"))
        pats.append(self.templates["Pat:RscOpOK"] % ("lxc2", "start_0"))
        pats.append(self.templates["Pat:RscOpOK"] % ("lxc-ms", "start_0"))
        pats.append(self.templates["Pat:RscOpOK"] % ("lxc-ms", "promote_0"))

        self.rsh(node, "/usr/share/pacemaker/tests/cts/lxc_autogen.sh -g -a -m -s -c %d &>/dev/null" % self.num_containers)
        self.set_timer("remoteSimpleInit")
        watch.lookforall()
        self.log_timer("remoteSimpleInit")
        if watch.unmatched:
            self.fail_string = "Unmatched patterns: %s" % (repr(watch.unmatched))
            self.failed = 1

    def cleanup_lxc_simple(self, node):

        pats = [ ]
        # if the test failed, attempt to clean up the cib and libvirt environment
        # as best as possible 
        if self.failed == 1:
            # restore libvirt and cib
            self.rsh(node, "/usr/share/pacemaker/tests/cts/lxc_autogen.sh -R &>/dev/null")
            self.rsh(node, "crm_resource -C -r container1 &>/dev/null")
            self.rsh(node, "crm_resource -C -r container2 &>/dev/null")
            self.rsh(node, "crm_resource -C -r lxc1 &>/dev/null")
            self.rsh(node, "crm_resource -C -r lxc2 &>/dev/null")
            self.rsh(node, "crm_resource -C -r lxc-ms &>/dev/null")
            time.sleep(20)
            return

        watch = self.create_watch(pats, 120)
        watch.setwatch()

        pats.append(self.templates["Pat:RscOpOK"] % ("container1", "stop_0"))
        pats.append(self.templates["Pat:RscOpOK"] % ("container2", "stop_0"))

        self.rsh(node, "/usr/share/pacemaker/tests/cts/lxc_autogen.sh -p &>/dev/null")
        self.set_timer("remoteSimpleCleanup")
        watch.lookforall()
        self.log_timer("remoteSimpleCleanup")

        if watch.unmatched:
            self.fail_string = "Unmatched patterns: %s" % (repr(watch.unmatched))
            self.failed = 1

        # cleanup libvirt
        self.rsh(node, "/usr/share/pacemaker/tests/cts/lxc_autogen.sh -R &>/dev/null")

    def __call__(self, node):
        '''Perform the 'RemoteLXC' test. '''
        self.incr("calls")

        ret = self.startall(None)
        if not ret:
            return self.failure("Setup failed, start all nodes failed.")

        rc = self.rsh(node, "/usr/share/pacemaker/tests/cts/lxc_autogen.sh -v &>/dev/null")
        if rc == 1:
            self.log("Environment test for lxc support failed.")
            return self.skipped()

        self.start_lxc_simple(node)
        self.cleanup_lxc_simple(node)

        self.debug("Waiting for the cluster to recover")
        self.CM.cluster_stable()

        if self.failed == 1:
            return self.failure(self.fail_string)

        return self.success()

    def errorstoignore(self):
        '''Return list of errors which should be ignored'''
        return [
            r"Updating failcount for ping",
            r"pengine.*: Recover (ping|lxc-ms|container)\s*\(.*\)",
            # The orphaned lxc-ms resource causes an expected transition error
            # that is a result of the pengine not having knowledge that the 
            # ms resource used to be a clone.  As a result it looks like that 
            # resource is running in multiple locations when it shouldn't... But in
            # this instance we know why this error is occurring and that it is expected.
            r"Calculated Transition .* /var/lib/pacemaker/pengine/pe-error",
            r"Resource lxc-ms .* is active on 2 nodes attempting recovery",
            r"Unknown operation: fail",
            r"(ERROR|error): sending stonithRA op to stonithd failed.",
        ]

AllTestClasses.append(RemoteLXC)


class RemoteDriver(CTSTest):

    def __init__(self, cm):
        CTSTest.__init__(self,cm)
        self.name = self.__class__.__name__
        self.is_docker_unsafe = 1
        self.start = StartTest(cm)
        self.startall = SimulStartLite(cm)
        self.stop = StopTest(cm)
        self.remote_rsc = "remote-rsc"
        self.cib_cmd = """cibadmin -C -o %s -X '%s' """
        self.reset()

    def reset(self):
        self.pcmk_started = 0
        self.failed = False
        self.fail_string = ""
        self.remote_node_added = 0
        self.remote_rsc_added = 0
        self.remote_use_reconnect_interval = self.Env.RandomGen.choice([True,False])

    def fail(self, msg):
        """ Mark test as failed. """

        self.failed = True

        # Always log the failure.
        self.logger.log(msg)

        # Use first failure as test status, as it's likely to be most useful.
        if not self.fail_string:
            self.fail_string = msg

    def get_othernode(self, node):
        for othernode in self.Env["nodes"]:
            if othernode == node:
                # we don't want to try and use the cib that we just shutdown.
                # find a cluster node that is not our soon to be remote-node.
                continue
            else:
                return othernode

    def del_rsc(self, node, rsc):
        othernode = self.get_othernode(node)
        rc = self.rsh(othernode, "crm_resource -D -r %s -t primitive" % (rsc))
        if rc != 0:
            self.fail("Removal of resource '%s' failed" % rsc)

    def add_rsc(self, node, rsc_xml):
        othernode = self.get_othernode(node)
        rc = self.rsh(othernode, self.cib_cmd % ("resources", rsc_xml))
        if rc != 0:
            self.fail("resource creation failed")

    def add_primitive_rsc(self, node):
        rsc_xml = """
<primitive class="ocf" id="%s" provider="heartbeat" type="Dummy">
    <operations>
      <op id="remote-rsc-monitor-interval-10s" interval="10s" name="monitor"/>
    </operations>
    <meta_attributes id="remote-meta_attributes"/>
</primitive>""" % (self.remote_rsc)
        self.add_rsc(node, rsc_xml)
        if not self.failed:
            self.remote_rsc_added = 1

    def add_connection_rsc(self, node):
        if self.remote_use_reconnect_interval:
            # use reconnect interval and make sure to set cluster-recheck-interval as well.
            rsc_xml = """
<primitive class="ocf" id="%s" provider="pacemaker" type="remote">
    <instance_attributes id="remote-instance_attributes"/>
        <instance_attributes id="remote-instance_attributes">
          <nvpair id="remote-instance_attributes-server" name="server" value="%s"/>
          <nvpair id="remote-instance_attributes-reconnect_interval" name="reconnect_interval" value="60s"/>
        </instance_attributes>
    <operations>
      <op id="remote-monitor-interval-60s" interval="60s" name="monitor"/>
      <op id="remote-name-start-interval-0-timeout-120" interval="0" name="start" timeout="60"/>
    </operations>
</primitive>""" % (self.remote_node, node)
            self.rsh(self.get_othernode(node), self.templates["SetCheckInterval"] % ("45s"))
        else:
            # not using reconnect interval
            rsc_xml = """
<primitive class="ocf" id="%s" provider="pacemaker" type="remote">
    <instance_attributes id="remote-instance_attributes"/>
        <instance_attributes id="remote-instance_attributes">
          <nvpair id="remote-instance_attributes-server" name="server" value="%s"/>
        </instance_attributes>
    <operations>
      <op id="remote-monitor-interval-60s" interval="60s" name="monitor"/>
      <op id="remote-name-start-interval-0-timeout-120" interval="0" name="start" timeout="120"/>
    </operations>
</primitive>""" % (self.remote_node, node)

        self.add_rsc(node, rsc_xml)
        if not self.failed:
            self.remote_node_added = 1

    def stop_pcmk_remote(self, node):
        # disable pcmk remote
        for i in range(10):
            rc = self.rsh(node, "service pacemaker_remote stop")
            if rc != 0:
                time.sleep(6)
            else:
                break

    def start_pcmk_remote(self, node):
        for i in range(10):
            rc = self.rsh(node, "service pacemaker_remote start")
            if rc != 0:
                time.sleep(6)
            else:
                self.pcmk_started = 1
                break

    def start_metal(self, node):
        pcmk_started = 0

        # make sure the resource doesn't already exist for some reason
        self.rsh(node, "crm_resource -D -r %s -t primitive" % (self.remote_rsc))
        self.rsh(node, "crm_resource -D -r %s -t primitive" % (self.remote_node))

        if not self.stop(node):
            self.fail("Failed to shutdown cluster node %s" % node)
            return

        self.start_pcmk_remote(node)

        if self.pcmk_started == 0:
            self.fail("Failed to start pacemaker_remote on node %s" % node)
            return

        # convert node to baremetal node now that it has shutdow the cluster stack
        pats = [ ]
        watch = self.create_watch(pats, 120)
        watch.setwatch()
        pats.append(self.templates["Pat:RscOpOK"] % (self.remote_node, "start"))
        pats.append(self.templates["Pat:DC_IDLE"])

        self.add_connection_rsc(node)

        self.set_timer("remoteMetalInit")
        watch.lookforall()
        self.log_timer("remoteMetalInit")
        if watch.unmatched:
            self.fail("Unmatched patterns: %s" % watch.unmatched)

    def migrate_connection(self, node):
        if self.failed:
            return

        pats = [ ]
        pats.append(self.templates["Pat:RscOpOK"] % (self.remote_node, "migrate_to"))
        pats.append(self.templates["Pat:RscOpOK"] % (self.remote_node, "migrate_from"))
        pats.append(self.templates["Pat:DC_IDLE"])
        watch = self.create_watch(pats, 120)
        watch.setwatch()

        (rc, lines) = self.rsh(node, "crm_resource -M -r %s" % (self.remote_node), None)
        if rc != 0:
            self.fail("failed to move remote node connection resource")
            return

        self.set_timer("remoteMetalMigrate")
        watch.lookforall()
        self.log_timer("remoteMetalMigrate")

        if watch.unmatched:
            self.fail("Unmatched patterns: %s" % watch.unmatched)
            return

    def fail_rsc(self, node):
        if self.failed:
            return

        watchpats = [ ]
        watchpats.append(self.templates["Pat:RscRemoteOpOK"] % (self.remote_rsc, "stop", self.remote_node))
        watchpats.append(self.templates["Pat:RscRemoteOpOK"] % (self.remote_rsc, "start", self.remote_node))
        watchpats.append(self.templates["Pat:DC_IDLE"])

        watch = self.create_watch(watchpats, 120)
        watch.setwatch()

        self.debug("causing dummy rsc to fail.")

        rc = self.rsh(node, "rm -f /var/run/resource-agents/Dummy*")

        self.set_timer("remoteRscFail")
        watch.lookforall()
        self.log_timer("remoteRscFail")
        if watch.unmatched:
            self.fail("Unmatched patterns during rsc fail: %s" % watch.unmatched)

    def fail_connection(self, node):
        if self.failed:
            return

        watchpats = [ ]
        watchpats.append(self.templates["Pat:FenceOpOK"] % self.remote_node)
        watchpats.append(self.templates["Pat:NodeFenced"] % self.remote_node)

        watch = self.create_watch(watchpats, 120)
        watch.setwatch()

        # force stop the pcmk remote daemon. this will result in fencing
        self.debug("Force stopped active remote node")
        self.stop_pcmk_remote(node)

        self.debug("Waiting for remote node to be fenced.")
        self.set_timer("remoteMetalFence")
        watch.lookforall()
        self.log_timer("remoteMetalFence")
        if watch.unmatched:
            self.fail("Unmatched patterns: %s" % watch.unmatched)
            return

        self.debug("Waiting for the remote node to come back up")
        self.CM.ns.WaitForNodeToComeUp(node, 120);

        pats = [ ]
        watch = self.create_watch(pats, 240)
        watch.setwatch()
        pats.append(self.templates["Pat:RscOpOK"] % (self.remote_node, "start"))
        if self.remote_rsc_added == 1:
            pats.append(self.templates["Pat:RscRemoteOpOK"] % (self.remote_rsc, "start", self.remote_node))

        # start the remote node again watch it integrate back into cluster.
        self.start_pcmk_remote(node)
        if self.pcmk_started == 0:
            self.fail("Failed to start pacemaker_remote on node %s" % node)
            return

        self.debug("Waiting for remote node to rejoin cluster after being fenced.")
        self.set_timer("remoteMetalRestart")
        watch.lookforall()
        self.log_timer("remoteMetalRestart")
        if watch.unmatched:
            self.fail("Unmatched patterns: %s" % watch.unmatched)
            return

    def add_dummy_rsc(self, node):
        if self.failed:
            return

        # verify we can put a resource on the remote node
        pats = [ ]
        watch = self.create_watch(pats, 120)
        watch.setwatch()
        pats.append(self.templates["Pat:RscRemoteOpOK"] % (self.remote_rsc, "start", self.remote_node))
        pats.append(self.templates["Pat:DC_IDLE"])

        # Add a resource that must live on remote-node
        self.add_primitive_rsc(node)

        # force that rsc to prefer the remote node. 
        (rc, line) = self.CM.rsh(node, "crm_resource -M -r %s -N %s -f" % (self.remote_rsc, self.remote_node), None)
        if rc != 0:
            self.fail("Failed to place remote resource on remote node.")
            return

        self.set_timer("remoteMetalRsc")
        watch.lookforall()
        self.log_timer("remoteMetalRsc")
        if watch.unmatched:
            self.fail("Unmatched patterns: %s" % watch.unmatched)

    def test_attributes(self, node):
        if self.failed:
            return

        # This verifies permanent attributes can be set on a remote-node. It also
        # verifies the remote-node can edit it's own cib node section remotely.
        (rc, line) = self.CM.rsh(node, "crm_attribute -l forever -n testattr -v testval -N %s" % (self.remote_node), None)
        if rc != 0:
            self.fail("Failed to set remote-node attribute. rc:%s output:%s" % (rc, line))
            return

        (rc, line) = self.CM.rsh(node, "crm_attribute -l forever -n testattr -Q -N %s" % (self.remote_node), None)
        if rc != 0:
            self.fail("Failed to get remote-node attribute")
            return

        (rc, line) = self.CM.rsh(node, "crm_attribute -l forever -n testattr -D -N %s" % (self.remote_node), None)
        if rc != 0:
            self.fail("Failed to delete remote-node attribute")
            return

    def cleanup_metal(self, node):
        if self.pcmk_started == 0:
            return

        pats = [ ]

        watch = self.create_watch(pats, 120)
        watch.setwatch()

        if self.remote_rsc_added == 1:
            pats.append(self.templates["Pat:RscOpOK"] % (self.remote_rsc, "stop"))
        if self.remote_node_added == 1:
            pats.append(self.templates["Pat:RscOpOK"] % (self.remote_node, "stop"))

        self.set_timer("remoteMetalCleanup")

        if self.remote_use_reconnect_interval:
            self.debug("Cleaning up re-check interval")
            self.rsh(self.get_othernode(node), self.templates["ClearCheckInterval"])

        if self.remote_rsc_added == 1:

            # Remove dummy resource added for remote node tests
            self.debug("Cleaning up dummy rsc put on remote node")
            self.rsh(node, "crm_resource -U -r %s" % self.remote_rsc)
            self.del_rsc(node, self.remote_rsc)

        if self.remote_node_added == 1:

            # Remove remote node's connection resource
            self.debug("Cleaning up remote node connection resource")
            self.rsh(node, "crm_resource -U -r %s" % (self.remote_node))
            self.del_rsc(node, self.remote_node)

        watch.lookforall()
        self.log_timer("remoteMetalCleanup")

        if watch.unmatched:
            self.fail("Unmatched patterns: %s" % watch.unmatched)

        self.stop_pcmk_remote(node)

        self.debug("Waiting for the cluster to recover")
        self.CM.cluster_stable()

        if self.remote_node_added == 1:
            # Remove remote node itself
            self.debug("Cleaning up node entry for remote node")
            self.rsh(self.get_othernode(node), "crm_node --force --remove %s" % self.remote_node)

    def setup_env(self, node):

        self.remote_node = "remote_%s" % (node)

        # we are assuming if all nodes have a key, that it is
        # the right key... If any node doesn't have a remote
        # key, we regenerate it everywhere.
        if self.rsh.exists_on_all("/etc/pacemaker/authkey", self.Env["nodes"]):
            return

        # create key locally
        (handle, keyfile) = tempfile.mkstemp(".cts")
        os.close(handle)
        devnull = open(os.devnull, 'wb')
        subprocess.check_call(["dd", "if=/dev/urandom", "of=%s" % keyfile, "bs=4096", "count=1"],
            stdout=devnull, stderr=devnull)
        devnull.close()

        # sync key throughout the cluster
        for node in self.Env["nodes"]:
            self.rsh(node, "mkdir -p --mode=0750 /etc/pacemaker")
            self.rsh.cp(keyfile, "root@%s:/etc/pacemaker/authkey" % node)
            self.rsh(node, "chgrp haclient /etc/pacemaker /etc/pacemaker/authkey")
            self.rsh(node, "chmod 0640 /etc/pacemaker/authkey")
        os.unlink(keyfile)

    def is_applicable(self):
        if not self.is_applicable_common():
            return False

        for node in self.Env["nodes"]:
            rc = self.rsh(node, "type pacemaker_remoted >/dev/null 2>&1")
            if rc != 0:
                return False
        return True

    def start_new_test(self, node):
        self.incr("calls")
        self.reset()

        ret = self.startall(None)
        if not ret:
            return self.failure("Setup failed, start all nodes failed.")

        self.setup_env(node)
        self.start_metal(node)
        self.add_dummy_rsc(node)

    def __call__(self, node):
        return self.failure("This base class is not meant to be called directly.")

    def errorstoignore(self):
        '''Return list of errors which should be ignored'''
        return [ """is running on remote.*which isn't allowed""",
                 """Connection terminated""",
                 """Failed to send remote""",
                ]

# RemoteDriver is just a base class for other tests, so it is not added to AllTestClasses


class RemoteBasic(RemoteDriver):

    def __call__(self, node):
        '''Perform the 'RemoteBaremetal' test. '''

        self.start_new_test(node)
        self.test_attributes(node)
        self.cleanup_metal(node)

        self.debug("Waiting for the cluster to recover")
        self.CM.cluster_stable()
        if self.failed:
            return self.failure(self.fail_string)

        return self.success()

AllTestClasses.append(RemoteBasic)

class RemoteStonithd(RemoteDriver):

    def __call__(self, node):
        '''Perform the 'RemoteStonithd' test. '''

        self.start_new_test(node)
        self.fail_connection(node)
        self.cleanup_metal(node)

        self.debug("Waiting for the cluster to recover")
        self.CM.cluster_stable()
        if self.failed:
            return self.failure(self.fail_string)

        return self.success()

    def is_applicable(self):
        if not RemoteDriver.is_applicable(self):
            return False

        if "DoFencing" in self.Env.keys():
            return self.Env["DoFencing"]

        return True

    def errorstoignore(self):
        ignore_pats = [
            r"Unexpected disconnect on remote-node",
            r"crmd.*:\s+error.*: Operation remote_.*_monitor",
            r"pengine.*:\s+Recover remote_.*\s*\(.*\)",
            r"Calculated Transition .* /var/lib/pacemaker/pengine/pe-error",
            r"error.*: Resource .*ocf::.* is active on 2 nodes attempting recovery",
        ]

        ignore_pats.extend(RemoteDriver.errorstoignore(self))
        return ignore_pats

AllTestClasses.append(RemoteStonithd)


class RemoteMigrate(RemoteDriver):

    def __call__(self, node):
        '''Perform the 'RemoteMigrate' test. '''

        self.start_new_test(node)
        self.migrate_connection(node)
        self.cleanup_metal(node)

        self.debug("Waiting for the cluster to recover")
        self.CM.cluster_stable()
        if self.failed:
            return self.failure(self.fail_string)

        return self.success()

AllTestClasses.append(RemoteMigrate)


class RemoteRscFailure(RemoteDriver):

    def __call__(self, node):
        '''Perform the 'RemoteRscFailure' test. '''

        self.start_new_test(node)

        # This is an important step. We are migrating the connection
        # before failing the resource. This verifies that the migration
        # has properly maintained control over the remote-node.
        self.migrate_connection(node)

        self.fail_rsc(node)
        self.cleanup_metal(node)

        self.debug("Waiting for the cluster to recover")
        self.CM.cluster_stable()
        if self.failed:
            return self.failure(self.fail_string)

        return self.success()

    def errorstoignore(self):
        ignore_pats = [
            r"pengine.*: Recover remote-rsc\s*\(.*\)",
        ]

        ignore_pats.extend(RemoteDriver.errorstoignore(self))
        return ignore_pats

AllTestClasses.append(RemoteRscFailure)

# vim:ts=4:sw=4:et:
