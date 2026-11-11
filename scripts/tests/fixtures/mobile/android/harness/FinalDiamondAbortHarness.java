import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class FinalDiamondAbortHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("owned final diamond aborts"); }
    static Core make(int route,Object seed,Object alternate) throws Throwable {
        if (route==0) return new Core(seed,alternate);
        if (route==1) return FinalDiamondAbortCaller.make(seed,alternate);
        MethodHandle handle=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Object.class,Object.class));
        if (route==2) return (Core)handle.invokeWithArguments(seed,alternate);
        return (Core)handle.invokeExact(seed,alternate);
    }
    static boolean has(int[] path,int length,int tag) { for(int i=0;i<length;i++) if(path[i]==tag)return true;return false; }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int[] failures={0,0,0,0,0,0,0,1,2,3,5,8,10,11,12,13,4,6,7,9,14,14,0,0,0,0,0,0,0,0,0,0};
        Core previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            Object seed=(mode==22||mode==23)?null:new Object();
            boolean alternatePath=mode==1||mode==4||mode==6||mode==19||mode==21||mode==23||mode==24;
            Object alternate=alternatePath?(mode==24?seed:new Object()):null;
            Object produced=mode==25?seed:new Object();
            Object last=mode==26?produced:new Object();
            Trace.events=0;Trace.partial=null;Trace.accepts=0;Trace.chooses=0;Trace.finishes=0;Trace.problems=0;
            Trace.acceptedSeed=null;Trace.chosenSeed=null;Trace.finishedSeed=null;
            Trace.accepted=mode!=2&&mode!=16;
            Trace.inlineAbort=mode==3||mode==17||mode==18;
            Trace.rejectAlternate=mode==4||mode==19;
            Trace.abortAfter=mode==5||mode==6||mode==20||mode==21;
            Trace.fail=failures[mode];
            Trace.failure=mode%2==0?new IllegalStateException("owned step"):new AssertionError("owned step");
            Trace.problemFailure=new IllegalArgumentException("owned branch");
            Trace.produced=produced;Trace.finalValue=last;Trace.replacement=new Object();
            Trace.stamp=0x12345678fedcba98L;
            Trace.changedStamp=mode==27?Long.MIN_VALUE:mode==28?Long.MAX_VALUE:mode==29?0:mode==30?0x8000000000000000L:0x6a5b4c3d2e1f9081L;
            Trace.otherStamp=~Trace.changedStamp;
            int[] path=!Trace.accepted?new int[]{1,2,3,4}:Trace.inlineAbort?new int[]{1,2,3,5,6,7}:Trace.rejectAlternate?new int[]{1,2,3,9}:alternatePath?(Trace.abortAfter?new int[]{1,2,3,10,11,12,13,14}:new int[]{1,2,3,10,11,12,13}):(Trace.abortAfter?new int[]{1,2,3,5,8,10,11,12,13,14}:new int[]{1,2,3,5,8,10,11,12,13});
            int length=path.length;
            if(Trace.fail!=0) for(int i=0;i<path.length;i++) if(path[i]==Trace.fail){length=i+1;break;}
            long events=0;for(int i=0;i<length;i++)events=(events<<4)|path[i];
            Core value=null;Throwable failure=null;
            try { value=make(route,seed,alternate); } catch(Throwable error) { failure=error; }
            require(Trace.events==events && Trace.partial!=null && Trace.partial!=previous);previous=Trace.partial;
            require(Trace.accepts==(has(path,length,3)?1:0) && Trace.chooses==(has(path,length,8)?1:0) && Trace.finishes==(has(path,length,11)?1:0));
            require(Trace.acceptedSeed==(has(path,length,3)?seed:null) && Trace.chosenSeed==(has(path,length,8)?seed:null) && Trace.finishedSeed==(has(path,length,11)?seed:null));
            int problems=0;for(int tag:new int[]{4,7,9,14})if(has(path,length,tag))problems++;
            require(Trace.problems==problems);
            require(Trace.partial.first==(has(path,length,2)?seed:null));
            require(Trace.partial.choice==(has(path,length,10)?(alternatePath?alternate:produced):null));
            require(Trace.partial.last==(has(path,length,12)?last:null));
            require(Trace.partial.stamp==(has(path,length,13)?Trace.changedStamp:0));
            boolean abrupt=!Trace.accepted||Trace.inlineAbort||Trace.rejectAlternate||Trace.abortAfter;
            Throwable expected=Trace.fail!=0?Trace.failure:abrupt?Trace.problemFailure:null;
            require(failure==expected && (expected==null?value==Trace.partial:value==null));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+events);
        }
    }
}
