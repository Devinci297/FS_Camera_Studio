#include "fscs/core/engine.hpp"

#include <algorithm>

namespace fscs::core {

bool validPerimeter(const std::vector<Vec3>& p) {
    if (p.size() < 3 || p.size() > 256) return false;
    const auto cross=[](const Vec3& a,const Vec3& b){return a.x*b.y-a.y*b.x;};
    double area=0;
    const auto u=p[1]-p[0],v=p[2]-p[0];const double det=cross(u,v);
    if(std::abs(det)<1e-7)return false;
    const double sx=(u.z*v.y-v.z*u.y)/det,sy=(u.x*v.z-v.x*u.z)/det;
    for (std::size_t i=0; i<p.size(); ++i) {
        if (!p[i].finite() || std::abs(p[i].x)>1000 || std::abs(p[i].y)>1000 || std::abs(p[i].z)>100) return false;
        const auto a=p[(i+1)%p.size()]-p[i];
        if (std::hypot(a.x,a.y)<.001 || std::abs(p[i].z-p[0].z-sx*(p[i].x-p[0].x)-sy*(p[i].y-p[0].y))>.001) return false;
        area+=cross(p[i],p[(i+1)%p.size()]);
        for (std::size_t j=i+1; j<p.size(); ++j) if (j!=(i+1)%p.size() && (j+1)%p.size()!=i) {
            const auto b=p[(j+1)%p.size()]-p[j],delta=p[j]-p[i];const double den=cross(a,b);
            if(std::abs(den)>1e-9) {
                const double t=cross(delta,b)/den,s=cross(delta,a)/den;
                if(t>=-1e-8 && t<=1+1e-8 && s>=-1e-8 && s<=1+1e-8)return false;
            } else if(std::abs(cross(delta,a))<1e-8) {
                const double len=a.x*a.x+a.y*a.y;
                const double t=(delta.x*a.x+delta.y*a.y)/len,s=t+(b.x*a.x+b.y*a.y)/len;
                if(std::max(std::min(t,s),0.)<=std::min(std::max(t,s),1.))return false;
            }
        }
    }
    return std::abs(area)>1e-4;
}

std::vector<Vec3> perimeterRoute(const Vec3& from, const Vec3& to, const std::vector<Vec3>& p, bool roundCorners) {
    if (!validPerimeter(p) || !from.finite() || !to.finite()) return {};
    const auto locate=[&](const Vec3& point) {
        for (std::size_t i=0; i<p.size(); ++i) {
            const auto a=p[i], d=p[(i+1)%p.size()]-a;
            const double u=clamp(((point.x-a.x)*d.x+(point.y-a.y)*d.y)/(d.x*d.x+d.y*d.y),0.,1.);
            if ((point-(a+d*u)).length()<.002) return static_cast<int>(i);
        }
        return -1;
    };
    const int a=locate(from), b=locate(to);
    if (a<0 || b<0) return {};
    if (a==b) return {from,to};
    std::vector<Vec3> forward{from}, backward{from};
    for (int i=(a+1)%p.size();;i=(i+1)%p.size()) {
        forward.push_back(p[i]); if(i==b)break;
    }
    for (int i=a;;i=(i+static_cast<int>(p.size())-1)%p.size()) {
        backward.push_back(p[i]); if(i==(b+1)%static_cast<int>(p.size()))break;
    }
    forward.push_back(to); backward.push_back(to);
    const auto length=[](const std::vector<Vec3>& route){double n=0;for(std::size_t i=1;i<route.size();++i)n+=(route[i]-route[i-1]).length();return n;};
    auto route=length(forward)<=length(backward)?forward:backward;
    route.erase(std::unique(route.begin(),route.end(),[](const Vec3& a,const Vec3& b){return (a-b).length()<1e-6;}),route.end());
    return roundCorners ? roundRouteCorners(route) : route;
}

std::vector<Vec3> roundRouteCorners(const std::vector<Vec3>& input) {
    std::vector<Vec3> route;
    for(const auto& p:input){
        if(!route.empty()&&(p-route.back()).length()<.0001)continue;
        while(route.size()>1){
            const auto a=route.back()-route[route.size()-2],b=p-route.back();
            if(dot(a,b)<0||cross(a,b).length()>1e-8*a.length()*b.length())break;
            route.pop_back();
        }
        route.push_back(p);
    }
    if(route.empty())return {};
    // Round corners within a 20 cm tube of the perimeter. The planner reserves
    // 25 cm beyond the requested clearance for these turns. Tiny mesh edges
    // no longer make the walker stop and restart at every vertex.
    std::vector<Vec3> rounded{route.front()};
    for(std::size_t i=1;i+1<route.size();++i){
        const auto incoming=route[i]-route[i-1],outgoing=route[i+1]-route[i];
        const double a=incoming.length(),b=outgoing.length();
        if(a<1e-6||b<1e-6)continue;
        const double radius=std::min({.2,a*.3,b*.3});
        const auto start=route[i]-incoming*(radius/a),end=route[i]+outgoing*(radius/b);
        rounded.push_back(start);
        for(int k=1;k<=24;++k){const double u=k/24.;rounded.push_back(start*((1-u)*(1-u))+route[i]*(2*u*(1-u))+end*(u*u));}
    }
    if(route.size()>1)rounded.push_back(route.back());
    return rounded;
}

bool CameraPose::withinLimits() const {
    return finite() && std::abs(positionFRD.x) <= kMaxPositionMeters && std::abs(positionFRD.y) <= kMaxPositionMeters &&
           std::abs(positionFRD.z) <= kMaxPositionMeters && fovRad >= kMinFovRad && fovRad <= kMaxFovRad &&
           std::abs(orientation.norm() - 1.0) < 1e-6;
}

double ease(Easing kind, double u) {
    u = clamp(u, 0.0, 1.0);
    switch (kind) {
        case Easing::Cut: return 1.0;
        case Easing::Smoothstep3: return u * u * (3.0 - 2.0 * u);
        case Easing::Smoothstep5: return u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
    }
    return 1.0;
}

Transition::Transition(const CameraPose& from, const CameraPose& to, const TransitionSpec& spec)
    : from_(from), to_(to), spec_(spec) {
    spec_.delaySeconds = clamp(spec_.delaySeconds, 0.0, kMaxDelaySeconds);
    if (spec_.easing == Easing::Cut) {
        spec_.durationSeconds = 0.0;
    } else {
        spec_.durationSeconds = clamp(spec_.durationSeconds, kMinTransitionSeconds, spec_.walking ? 600. : kMaxTransitionSeconds);
    }
    if(spec_.walking && spec_.easing!=Easing::Cut)prepareWalking();
}

void Transition::prepareWalking() {
    // Uniform distance samples remove mesh-density dependence. The speed plan
    // brakes before corners and bounds both forward and sideways acceleration.
    std::vector<Vec3> route;
    for(const auto& p:spec_.route)if(route.empty()||(p-route.back()).length()>1e-7)route.push_back(p);
    if(route.empty())route={from_.positionFRD,to_.positionFRD};
    std::vector<double> lengths(route.size(),0);
    for(std::size_t i=1;i<route.size();++i)lengths[i]=lengths[i-1]+(route[i]-route[i-1]).length();
    const double length=lengths.back();
    if(length<.01){
        // Coincident gear viewpoints are a head turn, not an out-and-back walk.
        spec_.durationSeconds=std::max({1.5,spec_.durationSeconds,angleBetween(from_.orientation,to_.orientation)/(30*kDegToRad)*1.875});
        spec_.route.clear();spec_.walking=false;spec_.easing=Easing::Smoothstep5;return;
    }
    const auto at=[&](double distance){
        distance=clamp(distance,0.,length);
        const auto end=std::upper_bound(lengths.begin(),lengths.end(),distance);
        const std::size_t i=std::min<std::size_t>(std::max<std::size_t>(1,end-lengths.begin()),route.size()-1);
        return lerp(route[i-1],route[i],(distance-lengths[i-1])/(lengths[i]-lengths[i-1]));
    };
    const int count=std::max(2,static_cast<int>(std::ceil(length/.02)));
    const double step=length/count,cruise=clamp(length/std::max(.1,spec_.durationSeconds-1.),.4,2.2);
    std::vector<Vec3> points;points.reserve(count+1);
    for(int i=0;i<=count;++i)points.push_back(at(i*step));
    std::vector<double> speed(count+1,cruise),time(count+1,0);
    for(int i=1;i<count;++i){
        const auto a=points[i]-points[i-1],b=points[i+1]-points[i];
        const double size=a.length()*b.length();
        const double turn=size>1e-12?std::acos(clamp(dot(a,b)/size,-1.,1.)):0;
        if(turn>1e-5)speed[i]=std::min(cruise,std::sqrt(.45*step/turn));
    }
    speed.front()=speed.back()=0;
    for(int i=1;i<=count;++i)speed[i]=std::min(speed[i],std::sqrt(speed[i-1]*speed[i-1]+2*.6*step));
    for(int i=count-1;i>=0;--i)speed[i]=std::min(speed[i],std::sqrt(speed[i+1]*speed[i+1]+2*.6*step));
    for(int i=1;i<=count;++i)time[i]=time[i-1]+2*step/(speed[i-1]+speed[i]);
    const double travelTime=time.back();
    walkingKeys_.push_back({0,from_});
    auto orientation=from_.orientation;
    double elapsed=0;
    while(elapsed<travelTime){
        const double next=std::min(travelTime,elapsed+1./60),dt=next-elapsed;elapsed=next;
        const auto end=std::upper_bound(time.begin(),time.end(),elapsed);
        const std::size_t i=std::min<std::size_t>(std::max<std::size_t>(1,end-time.begin()),count);
        const double t=elapsed-time[i-1],span=time[i]-time[i-1];
        const double distance=clamp((i-1)*step+speed[i-1]*t+.5*(speed[i]-speed[i-1])/span*t*t,0.,length);
        // The pilot looks into the upcoming bend before their feet turn. Use
        // a forward-biased sight line, rather than averaging past/future motion
        // (which makes the head lag behind a corner).
        const double lookAhead=clamp(.75+cruise*.45,.9,1.6);
        const auto direction=at(distance+lookAhead)-at(distance-.1);
        auto desired=direction.length()>1e-6?Quat::fromPbh({-3,0,std::atan2(direction.y,direction.x)*kRadToDeg}):orientation;
        const double blendDistance=std::min(2.,length*.4);
        if(distance<blendDistance)desired=slerpShortest(from_.orientation,desired,ease(Easing::Smoothstep5,distance/blendDistance));
        if(length-distance<blendDistance)desired=slerpShortest(to_.orientation,desired,ease(Easing::Smoothstep5,(length-distance)/blendDistance));
        const double angle=angleBetween(orientation,desired);
        orientation=slerpShortest(orientation,desired,std::min(-std::expm1(-dt/.35),angle>1e-9?45*kDegToRad*dt/angle:1.));
        walkingKeys_.push_back({elapsed,{at(distance),orientation,from_.fovRad+(to_.fovRad-from_.fovRad)*ease(Easing::Smoothstep5,distance/length)}});
    }
    // Finish the head turn while stationary before starting the dwell scan.
    // This also avoids a last-frame snap when the camera engine drops a transition.
    while(angleBetween(orientation,to_.orientation)>1e-6 && elapsed<travelTime+8){
        const double angle=angleBetween(orientation,to_.orientation),dt=1./60;
        orientation=slerpShortest(orientation,to_.orientation,std::min(-std::expm1(-dt/.35),45*kDegToRad*dt/angle));
        elapsed+=dt;walkingKeys_.push_back({elapsed,{to_.positionFRD,orientation,to_.fovRad}});
    }
    elapsed+=1./60;walkingKeys_.push_back({elapsed,to_});spec_.durationSeconds=elapsed;
}

void Transition::advance(double dt) {
    if (dt > 0.0 && std::isfinite(dt)) elapsed_ += dt;
}

CameraPose Transition::sample() const {
    const double moving = elapsed_ - spec_.delaySeconds;
    if (moving < 0.0) return from_;
    if (spec_.durationSeconds <= 0.0 || moving >= spec_.durationSeconds) return to_;
    if(!walkingKeys_.empty()){
        const auto end=std::upper_bound(walkingKeys_.begin(),walkingKeys_.end(),moving,[](double t,const WalkingKey& k){return t<k.time;});
        const auto& b=end==walkingKeys_.end()?walkingKeys_.back():*end;
        const auto& a=end==walkingKeys_.begin()?walkingKeys_.front():*(end-1);
        const double u=b.time>a.time?clamp((moving-a.time)/(b.time-a.time),0.,1.):0;
        return {lerp(a.pose.positionFRD,b.pose.positionFRD,u),slerpShortest(a.pose.orientation,b.pose.orientation,u),a.pose.fovRad+(b.pose.fovRad-a.pose.fovRad)*u};
    }
    const double progress=moving/spec_.durationSeconds;
    double e = ease(spec_.easing, progress);

    auto position=lerp(from_.positionFRD, to_.positionFRD, e);
    auto orientation=slerpShortest(from_.orientation,to_.orientation,e);
    if (spec_.route.size()>1) {
        double length=0;for(std::size_t i=1;i<spec_.route.size();++i)length+=(spec_.route[i]-spec_.route[i-1]).length();
        double remaining=e*length;
        for(std::size_t i=1;i<spec_.route.size();++i) {
            const double segment=(spec_.route[i]-spec_.route[i-1]).length();
            if (remaining<=segment || i+1==spec_.route.size()) {
                const double t=segment>0?clamp(remaining/segment,0.,1.):1.;
                const double blend=t;
                position=lerp(spec_.route[i-1],spec_.route[i],blend);

                break;
            }
            remaining-=segment;
        }
    }
    return {position, orientation,
            from_.fovRad + (to_.fovRad - from_.fovRad) * e};
}

bool Transition::finished() const { return elapsed_ >= spec_.delaySeconds + spec_.durationSeconds; }

bool TemporaryLook::active() const {
    return headingDeg != 0.0 || pitchDeg != 0.0 || translationHeadingFrame.length() > 0.0 || fovDeltaRad != 0.0;
}

CameraPose compose(const CameraPose& base, const TemporaryLook& look) {
    if (!look.active()) return base;
    Pbh angles = base.orientation.toPbh();
    const Quat headingFrame = Quat::fromPbh({0, 0, angles.headingDeg});
    angles.headingDeg = wrapDeg(angles.headingDeg + look.headingDeg);
    angles.pitchDeg = clamp(angles.pitchDeg + look.pitchDeg, -89.0, 89.0);
    CameraPose out;
    out.orientation = Quat::fromPbh(angles);
    out.positionFRD = base.positionFRD + headingFrame.rotate(look.translationHeadingFrame);
    out.fovRad = clamp(base.fovRad + look.fovDeltaRad, kMinFovRad, kMaxFovRad);
    return out;
}

namespace {
double approach(double current, double target, double alpha) { return current + (target - current) * alpha; }
double clampUnit(double v) { return std::isfinite(v) ? clamp(v, -1.0, 1.0) : 0.0; }
}  // namespace

void ManualMover::advance(double dt, const ManualInput& in, TemporaryLook& look) {
    if (!(dt > 0.0) || !std::isfinite(dt)) return;
    if (dt > kMaxGapSeconds) {
        // Do not integrate obsolete movement after a stall; reseed from rest.
        neutralize();
        return;
    }
    ManualInput target{clampUnit(in.lookRight), clampUnit(in.lookUp),    clampUnit(in.moveForward),
                       clampUnit(in.moveRight), clampUnit(in.moveUp),    clampUnit(in.zoomIn)};
    // Normalise multi-axis translation so diagonals are not faster.
    const double moveLen = Vec3{target.moveForward, target.moveRight, target.moveUp}.length();
    if (moveLen > 1.0) {
        target.moveForward /= moveLen;
        target.moveRight /= moveLen;
        target.moveUp /= moveLen;
    }
    const double alpha = -std::expm1(-kStep * speeds_.accelerationPerSec);
    accumulator_ += dt;
    while (accumulator_ >= kStep) {
        accumulator_ -= kStep;
        rate_.lookRight = approach(rate_.lookRight, target.lookRight, alpha);
        rate_.lookUp = approach(rate_.lookUp, target.lookUp, alpha);
        rate_.moveForward = approach(rate_.moveForward, target.moveForward, alpha);
        rate_.moveRight = approach(rate_.moveRight, target.moveRight, alpha);
        rate_.moveUp = approach(rate_.moveUp, target.moveUp, alpha);
        rate_.zoomIn = approach(rate_.zoomIn, target.zoomIn, alpha);

        look.headingDeg = wrapDeg(look.headingDeg + rate_.lookRight * speeds_.lookDegPerSec * kStep);
        look.pitchDeg = clamp(look.pitchDeg + rate_.lookUp * speeds_.lookDegPerSec * kStep, -178.0, 178.0);
        look.translationHeadingFrame +=
            Vec3{rate_.moveForward, rate_.moveRight, -rate_.moveUp} * (speeds_.moveMetersPerSec * (in.fast ? 4.0 : 1.0) * kStep);
        look.fovDeltaRad = clamp(look.fovDeltaRad - rate_.zoomIn * speeds_.zoomRadPerSec * kStep, -kMaxFovRad, kMaxFovRad);
    }
    // Snap tiny residual rates to zero so a released input fully stops.
    auto settle = [](double& r) { if (std::abs(r) < 1e-4) r = 0; };
    settle(rate_.lookRight); settle(rate_.lookUp); settle(rate_.moveForward);
    settle(rate_.moveRight); settle(rate_.moveUp); settle(rate_.zoomIn);
}

void CameraEngine::select(const CameraPose& target, const TransitionSpec& spec, bool resetLook) {
    if (resetLook) recenterLook();
    if (!base_) {
        base_ = target;
        transition_.reset();
        return;
    }
    const CameraPose from = transition_ ? transition_->sample() : *base_;
    transition_.emplace(from, target, spec);
    base_ = target;
    if (transition_->finished()) transition_.reset();
}

void CameraEngine::place(const CameraPose& pose) {
    base_ = pose;
    transition_.reset();
}

void CameraEngine::tick(double dt, const ManualInput& input, std::optional<Quat> movementOrientation) {
    if (transition_) {
        transition_->advance(dt);
        if (transition_->finished()) transition_.reset();
    }
    auto relative = input;
    if (const auto base = basePose()) {
        const auto orientation = movementOrientation.value_or(sample().pose.orientation);
        auto angles = orientation.toPbh();
        angles.headingDeg = wrapDeg(angles.headingDeg - base->orientation.toPbh().headingDeg);
        const auto direction = Quat::fromPbh(angles).rotate({input.moveForward,input.moveRight,0});
        relative.moveForward=direction.x; relative.moveRight=direction.y; relative.moveUp=input.moveUp-direction.z;
    }
    mover_.advance(dt, relative, look_);
}

void CameraEngine::freeze() { mover_.neutralize(); }

void CameraEngine::offsetPosition(const Vec3& delta) {
    if (!base_ || !delta.finite()) return;
    const auto old = look_.translationHeadingFrame;
    look_.translationHeadingFrame += delta;
    if (!sample().valid) look_.translationHeadingFrame = old;
}

void CameraEngine::offsetFov(double deltaRad) {
    const auto base = basePose();
    if (!base || !std::isfinite(deltaRad)) return;
    look_.fovDeltaRad = clamp(base->fovRad + look_.fovDeltaRad + deltaRad, kMinFovRad, kMaxFovRad) - base->fovRad;
}

void CameraEngine::offsetLook(double headingDeg, double pitchDeg) {
    if (!base_ || !std::isfinite(headingDeg) || !std::isfinite(pitchDeg)) return;
    look_.headingDeg = wrapDeg(look_.headingDeg + headingDeg);
    const auto base = basePose();
    const double pitch = base ? base->orientation.toPbh().pitchDeg : 0;
    look_.pitchDeg = clamp(pitch + look_.pitchDeg + pitchDeg, -89.0, 89.0) - pitch;
}

std::optional<CameraPose> CameraEngine::basePose() const {
    if (!base_) return std::nullopt;
    return transition_ ? transition_->sample() : *base_;
}

EngineSample CameraEngine::sample() const {
    const auto b = basePose();
    if (!b) return {};
    EngineSample s;
    s.pose = compose(*b, look_);
    s.pose.orientation = s.pose.orientation.normalized();
    s.valid = s.pose.withinLimits();
    return s;
}

void CameraEngine::reset() {
    base_.reset();
    transition_.reset();
    recenterLook();
}

}  // namespace fscs::core
