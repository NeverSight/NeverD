import api.*;
public class ProtectedAbstractNamesCaller {
    public static boolean call(Root receiver,Press scope,long bits) { return receiver.run(scope,bits); }
    public static boolean peer(Root receiver,Press scope,long bits) { return Peer.call(receiver,scope,bits); }
}
