#!/usr/bin/env python3
#
# Minimal mock of a DMTF Redfish Composition Service, used for
# exercising the libvirt 'redfish' driver without real hardware.
#
# It implements just enough of DSP0266/DSP0268 to cover:
#
#  - Session based authentication (and HTTP Basic auth)
#  - CompositionService with ResourceBlocks and ResourceZones
#  - Specific composition: POST /redfish/v1/Systems with
#    Links.ResourceBlocks, optionally completed asynchronously via a
#    task monitor (202 Accepted + Location)
#  - ComputerSystem.Reset power actions
#  - Decomposition: DELETE /redfish/v1/Systems/{id}
#
# State is kept in memory only.
#
# SPDX-License-Identifier: LGPL-2.1-or-later

import argparse
import base64
import json
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = "/redfish/v1"
SYSTEMS = ROOT + "/Systems"
COMPOSITION = ROOT + "/CompositionService"
BLOCKS = COMPOSITION + "/ResourceBlocks"
ZONES = COMPOSITION + "/ResourceZones"
SESSIONS = ROOT + "/SessionService/Sessions"
TASKS = ROOT + "/TaskService/Tasks"
MONITORS = ROOT + "/TaskService/TaskMonitors"

RESET_TYPES = ["On", "ForceOff", "GracefulShutdown", "GracefulRestart",
               "ForceRestart", "PowerCycle", "Nmi"]
BOOT_TARGETS = ["None", "Pxe", "Hdd", "Cd", "UefiHttp", "BiosSetup"]


def odata(path, otype, **kw):
    d = {"@odata.id": path, "@odata.type": otype}
    d.update(kw)
    return d


def ref(path):
    return {"@odata.id": path}


class State:
    def __init__(self, user, password, task_delay, hot_plug=False):
        self.lock = threading.Lock()
        self.hot_plug = hot_plug
        self.user = user
        self.password = password
        self.task_delay = task_delay
        self.sessions = {}
        self.systems = {}
        self.blocks = {}
        self.subresources = {}
        self.tasks = {}
        self.next_system = 1
        self.next_task = 1
        self.next_session = 1
        self._populate()

    def _block(self, bid, btypes, **links):
        path = BLOCKS + "/" + bid
        blk = odata(path, "#ResourceBlock.v1_4_0.ResourceBlock",
                    Id=bid, Name=bid,
                    ResourceBlockType=btypes,
                    Status={"State": "Enabled", "Health": "OK"},
                    CompositionStatus={"CompositionState": "Unused",
                                       "Reserved": False,
                                       "SharingCapable": False},
                    Links={"ComputerSystems": [],
                           "Zones": [ref(ZONES + "/1")]})
        for kind, items in links.items():
            blk[kind] = []
            for sid, res in items:
                sub = path + "/" + kind + "/" + sid
                res = dict(res)
                res.update({"@odata.id": sub, "Id": sid, "Name": sid})
                self.subresources[sub] = res
                blk[kind].append(ref(sub))
        self.blocks[bid] = blk

    def _populate(self):
        cpu = {"@odata.type": "#Processor.v1_18_0.Processor",
               "ProcessorType": "CPU",
               "Manufacturer": "Intel(R) Corporation",
               "Model": "Intel(R) Xeon(R) Platinum 8480+",
               "InstructionSet": "x86-64",
               "TotalCores": 56, "TotalThreads": 112}
        dimm = {"@odata.type": "#Memory.v1_17_0.Memory",
                "MemoryType": "DRAM", "CapacityMiB": 65536}
        for n in (1, 2, 3):
            self._block("Compute%d" % n, ["Compute"],
                        Processors=[("CPU0", cpu), ("CPU1", cpu)],
                        Memory=[("DIMM%d" % i, dimm) for i in range(4)])
        gpu = {"@odata.type": "#Processor.v1_18_0.Processor",
               "ProcessorType": "GPU",
               "Manufacturer": "NVIDIA Corporation",
               "Model": "H100 80GB HBM3",
               "ProcessorId": {"VendorId": "0x10de",
                               "IdentificationRegisters": "0x2330"}}
        for n in (1, 2):
            self._block("GPU%d" % n, ["Processor"],
                        Processors=[("GPU0", gpu)])
        cxl = {"@odata.type": "#Memory.v1_17_0.Memory",
               "MemoryType": "DRAM", "MemoryDeviceType": "DDR5",
               "CapacityMiB": 262144}
        self._block("CXLMem1", ["Memory"], Memory=[("Region0", cxl)])
        drive = {"@odata.type": "#Drive.v1_17_0.Drive",
                 "Protocol": "NVMeOverFabrics", "MediaType": "SSD",
                 "Manufacturer": "Samsung", "Model": "PM1743",
                 "SerialNumber": "S6CRNA0T", "CapacityBytes": 3840755982336}
        for n in (1, 2):
            self._block("NVMe%d" % n, ["Storage"],
                        Drives=[("Drive0", dict(drive,
                                                SerialNumber="S6CRNA0T%d" % n))])

        # A non-composed system managed by the same service; the driver
        # must not report it as a domain.
        self.systems["Mgmt1"] = odata(
            SYSTEMS + "/Mgmt1", "#ComputerSystem.v1_20_0.ComputerSystem",
            Id="Mgmt1", Name="rack-management-node",
            SystemType="Physical", PowerState="On",
            UUID=str(uuid.uuid4()),
            Status={"State": "Enabled", "Health": "OK"})

    # --- authentication --------------------------------------------------

    def authorized(self, headers):
        if self.user is None:
            return True
        token = headers.get("X-Auth-Token")
        if token and token in self.sessions.values():
            return True
        auth = headers.get("Authorization", "")
        if auth.startswith("Basic "):
            try:
                user, pw = base64.b64decode(auth[6:]).decode().split(":", 1)
            except Exception:
                return False
            return user == self.user and pw == self.password
        return False

    # --- composition -----------------------------------------------------

    def summarize(self, system):
        cores = threads = sockets = 0
        mem_mib = 0
        model = None
        for r in system["Links"]["ResourceBlocks"]:
            blk = self.blocks[r["@odata.id"].rsplit("/", 1)[1]]
            for p in blk.get("Processors", []):
                proc = self.subresources[p["@odata.id"]]
                if proc["ProcessorType"] != "CPU":
                    continue
                sockets += 1
                cores += proc["TotalCores"]
                threads += proc["TotalThreads"]
                model = proc["Model"]
            for m in blk.get("Memory", []):
                mem_mib += self.subresources[m["@odata.id"]]["CapacityMiB"]
        system["ProcessorSummary"] = {"Count": sockets, "CoreCount": cores,
                                      "LogicalProcessorCount": threads,
                                      "Model": model}
        system["MemorySummary"] = {"TotalSystemMemoryGiB": mem_mib / 1024}

    def compose(self, body):
        name = body.get("Name")
        if not name:
            return 400, error("PropertyMissing", "Name is required")
        refs = body.get("Links", {}).get("ResourceBlocks", [])
        if not refs:
            return 400, error("PropertyMissing",
                              "Links.ResourceBlocks is required")
        ids = []
        for r in refs:
            bid = r.get("@odata.id", "").rstrip("/").rsplit("/", 1)[-1]
            if bid not in self.blocks:
                return 400, error("ResourceMissingAtURI",
                                  "Unknown resource block %s" % r)
            st = self.blocks[bid]["CompositionStatus"]
            if st["CompositionState"] != "Unused" or st["Reserved"]:
                return 409, error("ResourceInUse",
                                  "Resource block %s is in use" % bid)
            ids.append(bid)
        if not any("Compute" in self.blocks[b]["ResourceBlockType"]
                   for b in ids):
            return 400, error("ActionParameterValueError",
                              "At least one Compute resource block is "
                              "required")
        for s in self.systems.values():
            if s["Name"] == name:
                return 409, error("ResourceAlreadyExists",
                                  "A system named '%s' exists" % name)

        sid = "Composed%d" % self.next_system
        self.next_system += 1
        path = SYSTEMS + "/" + sid
        boot = {"BootSourceOverrideEnabled": "Disabled",
                "BootSourceOverrideTarget": "None",
                "BootSourceOverrideTarget@Redfish.AllowableValues":
                    BOOT_TARGETS}
        boot.update({k: v for k, v in body.get("Boot", {}).items()
                     if k in ("BootSourceOverrideEnabled",
                              "BootSourceOverrideTarget")})
        system = odata(path, "#ComputerSystem.v1_20_0.ComputerSystem",
                       Id=sid, Name=name, SystemType="Composed",
                       Description=body.get("Description", ""),
                       UUID=str(uuid.uuid4()), PowerState="Off",
                       Status={"State": "Enabled", "Health": "OK"},
                       Boot=boot,
                       Actions={"#ComputerSystem.Reset": {
                           "target": path + "/Actions/ComputerSystem.Reset",
                           "ResetType@Redfish.AllowableValues": RESET_TYPES}},
                       Links={"ResourceBlocks": [ref(BLOCKS + "/" + b)
                                                 for b in ids]})
        for b in ids:
            blk = self.blocks[b]
            blk["CompositionStatus"]["CompositionState"] = "Composed"
            blk["Links"]["ComputerSystems"] = [ref(path)]
        self.summarize(system)

        if self.task_delay <= 0:
            self.systems[sid] = system
            return 201, system, {"Location": path}

        # Asynchronous: the system appears once the task completes,
        # mimicking optical link training / memory init taking a while.
        tid = str(self.next_task)
        self.next_task += 1
        task = odata(TASKS + "/" + tid, "#Task.v1_7_0.Task",
                     Id=tid, Name="Compose " + name,
                     TaskState="Running", PercentComplete=0,
                     TaskMonitor=MONITORS + "/" + tid,
                     StartTime=time.strftime("%Y-%m-%dT%H:%M:%SZ"))
        self.tasks[tid] = {"task": task, "system": system,
                           "done_at": time.time() + self.task_delay}
        return 202, task, {"Location": MONITORS + "/" + tid}

    def poll_task(self, tid):
        t = self.tasks.get(tid)
        if t is None:
            return None
        task = t["task"]
        if task["TaskState"] == "Running":
            if time.time() >= t["done_at"]:
                task["TaskState"] = "Completed"
                task["PercentComplete"] = 100
                task["TaskStatus"] = "OK"
                self.systems[t["system"]["Id"]] = t["system"]
            else:
                total = self.task_delay
                left = t["done_at"] - time.time()
                task["PercentComplete"] = int(100 * (total - left) / total)
        return t

    def set_blocks(self, sid, refs):
        """Replace the resource blocks of a composed system (PATCH of
        Links.ResourceBlocks), as DSP2050 allows for adding or removing
        blocks of an existing system."""
        system = self.systems.get(sid)
        if system is None:
            return 404, error("ResourceMissingAtURI", "No such system")
        if system["SystemType"] != "Composed":
            return 400, error("ActionNotSupported",
                              "Only composed systems can be changed")
        if system["PowerState"] != "Off" and not self.hot_plug:
            return 409, error("ResourceInUse",
                              "System must be powered off to change its "
                              "resource blocks")
        ids = []
        for r in refs:
            bid = r.get("@odata.id", "").rstrip("/").rsplit("/", 1)[-1]
            if bid not in self.blocks:
                return 400, error("ResourceMissingAtURI",
                                  "Unknown resource block %s" % r)
            if bid in ids:
                return 400, error("PropertyValueConflict",
                                  "Resource block %s listed twice" % bid)
            ids.append(bid)
        mine = [r["@odata.id"].rsplit("/", 1)[1]
                for r in system["Links"]["ResourceBlocks"]]
        for bid in ids:
            if bid not in mine and \
                    self.blocks[bid]["CompositionStatus"][
                        "CompositionState"] != "Unused":
                return 409, error("ResourceInUse",
                                  "Resource block %s is in use" % bid)
        if not any("Compute" in self.blocks[b]["ResourceBlockType"]
                   for b in ids):
            return 400, error("PropertyValueConflict",
                              "At least one Compute resource block is "
                              "required")
        path = system["@odata.id"]
        for bid in mine:
            if bid not in ids:
                blk = self.blocks[bid]
                blk["CompositionStatus"]["CompositionState"] = "Unused"
                blk["Links"]["ComputerSystems"] = []
        for bid in ids:
            blk = self.blocks[bid]
            blk["CompositionStatus"]["CompositionState"] = "Composed"
            blk["Links"]["ComputerSystems"] = [ref(path)]
        system["Links"]["ResourceBlocks"] = [ref(BLOCKS + "/" + b)
                                             for b in ids]
        self.summarize(system)
        return 200, system

    def decompose(self, sid):
        system = self.systems.get(sid)
        if system is None:
            return 404, error("ResourceMissingAtURI", "No such system")
        if system["SystemType"] != "Composed":
            return 400, error("ActionNotSupported",
                              "Only composed systems can be deleted")
        for r in system["Links"]["ResourceBlocks"]:
            blk = self.blocks[r["@odata.id"].rsplit("/", 1)[1]]
            blk["CompositionStatus"]["CompositionState"] = "Unused"
            blk["Links"]["ComputerSystems"] = []
        del self.systems[sid]
        return 204, None

    def reset(self, sid, body):
        system = self.systems.get(sid)
        if system is None:
            return 404, error("ResourceMissingAtURI", "No such system")
        rt = body.get("ResetType")
        if rt not in RESET_TYPES:
            return 400, error("ActionParameterNotSupported",
                              "ResetType %s not supported" % rt)
        if rt in ("On", "GracefulRestart", "ForceRestart", "PowerCycle"):
            system["PowerState"] = "On"
        elif rt in ("ForceOff", "GracefulShutdown"):
            system["PowerState"] = "Off"
        return 204, None

    # --- reads -----------------------------------------------------------

    def collection(self, path, otype, members):
        return odata(path, otype, Name=path.rsplit("/", 1)[1],
                     Members=[ref(m) for m in members],
                     **{"Members@odata.count": len(members)})

    def get(self, path):
        if path == ROOT:
            return odata(ROOT, "#ServiceRoot.v1_16_0.ServiceRoot",
                         Id="RootService", Name="Mock Composition Service",
                         RedfishVersion="1.19.0",
                         UUID="92384634-2938-2342-8820-489239905423",
                         Product="libvirt Redfish mock",
                         Vendor="libvirt",
                         Systems=ref(SYSTEMS),
                         CompositionService=ref(COMPOSITION),
                         SessionService=ref(ROOT + "/SessionService"),
                         TaskService=ref(ROOT + "/TaskService"),
                         Links={"Sessions": ref(SESSIONS)})
        if path == SYSTEMS:
            c = self.collection(path,
                                "#ComputerSystemCollection."
                                "ComputerSystemCollection",
                                [s["@odata.id"]
                                 for s in self.systems.values()])
            c["@Redfish.CollectionCapabilities"] = {
                "Capabilities": [{"CapabilitiesObject":
                                  ref(SYSTEMS + "/Capabilities"),
                                  "UseCase": "ComputerSystemComposition",
                                  "Links": {"TargetCollection":
                                            ref(SYSTEMS)}}]}
            return c
        if path.startswith(SYSTEMS + "/"):
            return self.systems.get(path.rsplit("/", 1)[1])
        if path == COMPOSITION:
            return odata(path,
                         "#CompositionService.v1_2_2.CompositionService",
                         Id="CompositionService",
                         Name="Composition Service",
                         ServiceEnabled=True,
                         AllowOverprovisioning=False,
                         ResourceBlocks=ref(BLOCKS),
                         ResourceZones=ref(ZONES))
        if path == BLOCKS:
            return self.collection(path,
                                   "#ResourceBlockCollection."
                                   "ResourceBlockCollection",
                                   [b["@odata.id"]
                                    for b in self.blocks.values()])
        if path.startswith(BLOCKS + "/"):
            rest = path[len(BLOCKS) + 1:]
            if "/" not in rest:
                return self.blocks.get(rest)
            return self.subresources.get(path)
        if path == ZONES:
            return self.collection(path, "#ZoneCollection.ZoneCollection",
                                   [ZONES + "/1"])
        if path == ZONES + "/1":
            return odata(path, "#Zone.v1_6_1.Zone", Id="1",
                         Name="Rack 1 fabric",
                         Links={"ResourceBlocks":
                                [ref(b["@odata.id"])
                                 for b in self.blocks.values()]})
        if path.startswith(TASKS + "/"):
            t = self.poll_task(path.rsplit("/", 1)[1])
            return t and t["task"]
        if path == ROOT + "/SessionService":
            return odata(path, "#SessionService.v1_1_9.SessionService",
                         Id="SessionService", Name="Session Service",
                         ServiceEnabled=True, Sessions=ref(SESSIONS))
        return None


def error(code, message):
    return {"error": {"code": "Base.1.16." + code, "message": message,
                      "@Message.ExtendedInfo": [{
                          "MessageId": "Base.1.16." + code,
                          "Message": message}]}}


class Handler(BaseHTTPRequestHandler):
    server_version = "RedfishMock/1.0"
    protocol_version = "HTTP/1.1"

    @property
    def state(self):
        return self.server.state

    def log_message(self, fmt, *args):
        if not self.server.quiet:
            super().log_message(fmt, *args)

    def reply(self, status, body=None, headers=None):
        data = b""
        if body is not None:
            data = json.dumps(body, indent=2).encode()
        self.send_response(status)
        self.send_header("OData-Version", "4.0")
        if body is not None:
            self.send_header("Content-Type",
                             "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def path_only(self):
        p = self.path.split("?", 1)[0].rstrip("/")
        return p or "/"

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if not n:
            return {}
        try:
            return json.loads(self.rfile.read(n))
        except ValueError:
            return None

    def check_auth(self, path):
        if path in (ROOT, "/redfish") or self.state.authorized(self.headers):
            return True
        self.reply(401, error("NoValidSession", "Authentication required"),
                   {"WWW-Authenticate": 'Basic realm="redfish"'})
        return False

    def dispatch(self, method):
        path = self.path_only()
        st = self.state
        body = None
        if method in ("POST", "PATCH"):
            body = self.body()
            if body is None:
                return self.reply(400, error("MalformedJSON",
                                             "Malformed JSON body"))

        if method == "POST" and path == SESSIONS:
            if (body.get("UserName") != st.user or
                    body.get("Password") != st.password):
                return self.reply(401, error("NoValidSession",
                                             "Invalid credentials"))
            with st.lock:
                sid = str(st.next_session)
                st.next_session += 1
                token = uuid.uuid4().hex
                st.sessions[sid] = token
            loc = SESSIONS + "/" + sid
            return self.reply(201, odata(loc, "#Session.v1_7_0.Session",
                                         Id=sid, UserName=st.user),
                              {"X-Auth-Token": token, "Location": loc})

        if not self.check_auth(path):
            return

        with st.lock:
            if method == "GET":
                if path.startswith(MONITORS + "/"):
                    t = st.poll_task(path.rsplit("/", 1)[1])
                    if t is None:
                        return self.reply(404, error("ResourceMissingAtURI",
                                                     "No such task"))
                    task = t["task"]
                    if task["TaskState"] == "Running":
                        return self.reply(202, task,
                                          {"Location": path,
                                           "Retry-After": "1"})
                    sys_ = t["system"]
                    return self.reply(201, sys_,
                                      {"Location": sys_["@odata.id"]})
                res = st.get(path)
                if res is None:
                    return self.reply(404, error("ResourceMissingAtURI",
                                                 "No resource at " + path))
                return self.reply(200, res)

            if method == "POST" and path == SYSTEMS:
                r = st.compose(body)
                return self.reply(*r)

            if (method == "POST" and path.startswith(SYSTEMS + "/") and
                    path.endswith("/Actions/ComputerSystem.Reset")):
                sid = path[len(SYSTEMS) + 1:].split("/", 1)[0]
                return self.reply(*st.reset(sid, body))

            if method == "PATCH" and path.startswith(SYSTEMS + "/"):
                system = st.systems.get(path.rsplit("/", 1)[1])
                if system is None:
                    return self.reply(404, error("ResourceMissingAtURI",
                                                 "No such system"))
                blocks = body.get("Links", {}).get("ResourceBlocks")
                if blocks is not None:
                    return self.reply(*st.set_blocks(
                        path.rsplit("/", 1)[1], blocks))
                for k, v in body.get("Boot", {}).items():
                    if k in ("BootSourceOverrideEnabled",
                             "BootSourceOverrideTarget"):
                        system["Boot"][k] = v
                return self.reply(200, system)

            if method == "DELETE" and path.startswith(SESSIONS + "/"):
                st.sessions.pop(path.rsplit("/", 1)[1], None)
                return self.reply(204)

            if method == "DELETE" and path.startswith(SYSTEMS + "/"):
                return self.reply(*st.decompose(path.rsplit("/", 1)[1]))

        if st.get(path) is not None:
            return self.reply(405, error("OperationNotAllowed",
                                         "%s not allowed on %s" %
                                         (method, path)))
        return self.reply(404, error("ResourceMissingAtURI",
                                     "No resource at " + path))

    def do_GET(self):
        self.dispatch("GET")

    def do_POST(self):
        self.dispatch("POST")

    def do_PATCH(self):
        self.dispatch("PATCH")

    def do_DELETE(self):
        self.dispatch("DELETE")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000,
                    help="TCP port to listen on, 0 picks a free one")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="password")
    ap.add_argument("--no-auth", action="store_true",
                    help="do not require authentication")
    ap.add_argument("--task-delay", type=float, default=2.0,
                    help="seconds an async composition takes; 0 makes "
                         "composition synchronous (201 Created)")
    ap.add_argument("--hot-plug", action="store_true",
                    help="allow changing the resource blocks of a system "
                         "that is powered on (by default it must be off)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    srv.state = State(None if args.no_auth else args.user,
                      args.password, args.task_delay, args.hot_plug)
    srv.quiet = args.quiet
    srv.daemon_threads = True
    print("listening on http://%s:%d" % srv.server_address[:2], flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
