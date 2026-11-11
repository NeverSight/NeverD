import fixture.*;
public final class ScopedNullDelegationCaller {
    public static <E> Link<E> shorter(E first,Peer<E> next){return new Link<E>(first,next);}
    public static <E> Link<E> longer(E first,E second,Peer<E> next){return new Link<E>(first,second,next);}
}
