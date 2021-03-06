/**
 * Tests that --repair on WiredTiger correctly and gracefully handles corrupt metadata files.
 *
 * @tags: [requires_wiredtiger,requires_journaling]
 */

(function() {

    load('jstests/disk/libs/wt_file_helper.js');

    const baseName = "wt_repair_corrupt_metadata";
    const collName = "test";
    const dbpath = MongoRunner.dataPath + baseName + "/";

    /**
     * This test runs repair using a version of the WiredTiger.turtle file that has checkpoint
     * information before the collection was created. The turtle file contains checkpoint
     * information about the WiredTiger.wt file, so if these two files become out of sync,
     * WiredTiger will have to attempt a salvage operation on the .wt file and rebuild the .turtle
     * file.
     *
     * The expectation is that the metadata salvage will be successful, and that the collection will
     * be recreated with all of its data.
     */
    let runTest = function(mongodOptions) {
        // Unfortunately using --nojournal triggers a WT_PANIC and aborts in debug builds, which the
        // following test case can exercise.
        // TODO: This return can be removed once WT-4310 is completed.
        if (db.adminCommand('buildInfo').debug && mongodOptions.hasOwnProperty('nojournal')) {
            jsTestLog(
                "Skipping test case because this is a debug build and --nojournal was provided.");
            return;
        }

        resetDbpath(dbpath);
        jsTestLog("Running test with args: " + tojson(mongodOptions));

        const turtleFile = dbpath + "WiredTiger.turtle";
        const turtleFileWithoutCollection = dbpath + "WiredTiger.turtle.1";

        let mongod = startMongodOnExistingPath(dbpath, mongodOptions);

        // Force a checkpoint and make a copy of the turtle file.
        assert.commandWorked(mongod.getDB(baseName).adminCommand({fsync: 1}));
        jsTestLog("Making copy of metadata file before creating the collection: " +
                  turtleFileWithoutCollection);
        copyFile(turtleFile, turtleFileWithoutCollection);

        let testColl = mongod.getDB(baseName)[collName];
        assert.commandWorked(testColl.insert({a: 1}));

        // Force another checkpoint before a clean shutdown.
        assert.commandWorked(mongod.getDB(baseName).adminCommand({fsync: 1}));
        MongoRunner.stopMongod(mongod);

        // Guarantee the turtle files changed between checkpoints.
        assert.neq(md5sumFile(turtleFileWithoutCollection), md5sumFile(turtleFile));

        jsTestLog("Replacing metadata file with a version before the collection existed.");
        removeFile(turtleFile);
        copyFile(turtleFileWithoutCollection, turtleFile);

        assertRepairSucceeds(dbpath, mongod.port, mongodOptions);

        mongod = startMongodOnExistingPath(dbpath, mongodOptions);
        testColl = mongod.getDB(baseName)[collName];

        // The collection exists depite using an older turtle file because salvage is able to find
        // the table in the WiredTiger.wt file.
        assert(testColl.exists());
        // We can assert that the data exists because the salvage only took place on the metadata,
        // not the data.
        assert.eq(testColl.find({}).itcount(), 1);
        MongoRunner.stopMongod(mongod);
    };

    // Repair may behave differently with journaling enabled or disabled, but the end result should
    // be the same.
    runTest({journal: ""});
    runTest({nojournal: ""});
})();
