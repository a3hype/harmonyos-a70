# Adds discovery retries to the A70 sensor VDI. Run from the workspace root, after build_a70_vdi.ps1 has
# produced the base file. build_a70_vdi.ps1 regenerates the source, so it calls this script too.
import sys

PATH = "coproc/sensors_a70/a70_sensors_vdi.cpp"
src = open(PATH, encoding="utf-8", newline="").read()

OLD_DISCOVER = """        for (auto &it : streams_) {
            Stream *s = it.second;
            Suid id;
            for (int tries = 0; tries < 3 && !id.Valid(); tries++) {
                id = LookupSuid(node, port, s->def->dataType);
            }
            std::lock_guard<std::mutex> l(mu_);
            s->suid = id;
            if (id.Valid()) {
                LOGI("%s: SUID %016llx%016llx", s->def->dataType, (unsigned long long)id.hi,
                     (unsigned long long)id.lo);
            } else {
                LOGI("%s: not available on this phone%s", s->def->dataType,
                     s->def->kind == VIRTUAL_GRAVITY ? " (computed from accel)" : "");
            }
        }
        std::lock_guard<std::mutex> l(mu_);
        discovered_ = true;
        discoverCv_.notify_all();
"""
NEW_DISCOVER = """        /* a70: the service answers before hexagonrpcd has let the physical sensors
         * register, and those early lookups say "none". Keep asking for 5 minutes. */
        for (int pass = 0; pass < 100; pass++) {
            bool missing = false;
            for (auto &it : streams_) {
                Stream *s = it.second;
                {
                    std::lock_guard<std::mutex> l(mu_);
                    if (s->suid.Valid()) {
                        continue;
                    }
                }
                Suid id = LookupSuid(node, port, s->def->dataType);
                std::lock_guard<std::mutex> l(mu_);
                s->suid = id;
                if (id.Valid()) {
                    LOGI("%s: SUID %016llx%016llx (pass %d)", s->def->dataType, (unsigned long long)id.hi,
                         (unsigned long long)id.lo, pass);
                } else {
                    missing = true;
                }
            }
            {
                std::lock_guard<std::mutex> l(mu_);
                if (!discovered_) {
                    discovered_ = true;
                    discoverCv_.notify_all();
                }
            }
            if (!missing) {
                LOGI("all sensors found (pass %d)", pass);
                return;
            }
            sleep(3);
        }
        LOGE("some sensors never appeared");
"""
OLD_STREAM = """        Suid suid;
        {
            std::lock_guard<std::mutex> l(mu_);
            suid = s->suid;
        }
        if (!suid.Valid()) {
            return;
        }
"""
NEW_STREAM = """        Suid suid;
        while (!stop) {
            {
                std::lock_guard<std::mutex> l(mu_);
                suid = s->suid;
            }
            if (suid.Valid()) {
                break;
            }
            sleep(1);
        }
        if (stop) {
            return;
        }
"""
for old, new in ((OLD_DISCOVER, NEW_DISCOVER), (OLD_STREAM, NEW_STREAM)):
    if src.count(old) != 1:
        sys.exit("pattern not found exactly once:\n" + old[:80])
    src = src.replace(old, new)
open(PATH, "w", encoding="utf-8", newline="").write(src)
print("patched")
