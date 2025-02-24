#ifndef SHARE_GC_G1_G1DRAINLOCALQUEUE_HPP
#define SHARE_GC_G1_G1DRAINLOCALQUEUE_HPP

class DrainLocalQueues {
public:
    DrainLocalQueues() {}

    static void perform();
};

#endif