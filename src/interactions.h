#pragma once

#include "intersections.h"

__host__ __device__ void computeRayIntersection(Geom* geoms, int geoms_size, Ray ray,
    ShadeableIntersection& intersection) {
    float t;
    glm::vec3 intersect_point;
    glm::vec3 normal;
    glm::vec3 tangent;
    float t_min = FLT_MAX;
    int hit_geom_index = -1;
    bool outside = true;

    glm::vec3 tmp_intersect;
    glm::vec3 tmp_normal;
    glm::vec3 tmp_tangent;

    // naive parse through global geoms

    for (int i = 0; i < geoms_size; i++)
    {
        Geom& geom = geoms[i];

        if (geom.type == CUBE)
        {
            t = boxIntersectionTest(geom, ray, tmp_intersect, tmp_normal, tmp_tangent, outside);
        }
        else if (geom.type == SPHERE)
        {
            t = sphereIntersectionTest(geom, ray, tmp_intersect, tmp_normal, tmp_tangent, outside);
        }
        // TODO: add more intersection tests here... triangle? metaball? CSG?

        // Compute the minimum t from the intersection tests to determine what
        // scene geometry object was hit first.
        if (t > 0.0f && t_min > t)
        {
            t_min = t;
            hit_geom_index = i;
            intersect_point = tmp_intersect;
            normal = tmp_normal;
            tangent = tmp_tangent;
        }
    }

    if (hit_geom_index == -1)
    {
        intersection.t = -1.0f;
    }
    else
    {
        //The ray hits something
        intersection.t = t_min;
        intersection.materialId = geoms[hit_geom_index].materialid;
        intersection.geomId = hit_geom_index;
        intersection.surfaceNormal = normal;
        intersection.surfaceTangent = tangent;
    }
}

__host__ __device__
glm::vec2 concentricSampleDisk(const glm::vec2& sample)
{
    glm::vec2 uOffset = 2.f * sample - glm::vec2(1.);
    if (uOffset.x == 0 && uOffset.y == 0) return glm::vec2(0, 0);
    float theta, r;
    if (std::abs(uOffset.x) > std::abs(uOffset.y)) {
        r = uOffset.x;
        theta = (PI / 4.0) * (uOffset.y / uOffset.x);
    }
    else {
        r = uOffset.y;
        theta = (PI / 2.0) - (PI / 4.0) * (uOffset.x / uOffset.y);
    }
    return r * glm::vec2(std::cos(theta), std::sin(theta));
}

// CHECKITOUT
/**
 * Computes a cosine-weighted random direction in a hemisphere.
 * Used for diffuse lighting.
 */
__host__ __device__
glm::vec3 calculateRandomDirectionInHemisphere(
        glm::vec3 normal, thrust::default_random_engine &rng, float& pdf) {
    thrust::uniform_real_distribution<float> u01(0, 1);

    float up = sqrt(u01(rng)); // cos(theta)
    float over = sqrt(1 - up * up); // sin(theta)
    float around = u01(rng) * TWO_PI;

    pdf = up / PI;

    // Find a direction that is not the normal based off of whether or not the
    // normal's components are all equal to sqrt(1/3) or whether or not at
    // least one component is less than sqrt(1/3). Learned this trick from
    // Peter Kutz.

    glm::vec3 directionNotNormal;
    if (abs(normal.x) < SQRT_OF_ONE_THIRD) {
        directionNotNormal = glm::vec3(1, 0, 0);
    } else if (abs(normal.y) < SQRT_OF_ONE_THIRD) {
        directionNotNormal = glm::vec3(0, 1, 0);
    } else {
        directionNotNormal = glm::vec3(0, 0, 1);
    }

    // Use not-normal direction to generate two perpendicular directions
    glm::vec3 perpendicularDirection1 =
        glm::normalize(glm::cross(normal, directionNotNormal));
    glm::vec3 perpendicularDirection2 =
        glm::normalize(glm::cross(normal, perpendicularDirection1));

    return up * normal
        + cos(around) * over * perpendicularDirection1
        + sin(around) * over * perpendicularDirection2;
}

__host__ __device__ float powerHeuristic(int nf, float fPdf, int ng, float gPdf) {
    float f = nf * fPdf, g = ng * gPdf;
    return (f * f) / (g * g + f * f);
}

__host__ __device__ float fresnelDielectricEval(float cosi, float etai, float etat) {
    if (cosi > 0.0f) {
        float tmp = etai;
        etai = etat;
        etat = tmp;
    }

    cosi = abs(cosi);

    float sint = (etai / etat) * sqrtf(fmaxf(0.0f, 1.0f - cosi * cosi));
    if (sint >= 1.0f) {
        // total internal reflection
        return 1.0;
    }
    
    float cost = sqrtf(fmaxf(0.0f, 1.0f - sint * sint));

    float Rparl = ((etat * cosi) - (etai * cost)) / ((etat * cosi) + (etai * cost));
    float Rperp = ((etai * cosi) - (etat * cost)) / ((etai * cosi) + (etat * cost));
    float Re = (Rparl * Rparl + Rperp * Rperp) / 2.0f;

    return Re;
}

/*Sample material color*/
__host__ __device__ glm::vec3 sampleSpecularReflectMaterial(
    const Material& m, const glm::vec3& normal, const glm::vec3& wo, glm::vec3& wi) {
    wi = glm::normalize(glm::reflect(wo, normal));
    return m.specular.color / abs(glm::dot(glm::normalize(wi), normal));
}

__host__ __device__ glm::vec3 sampleSpecularTransmissionMaterial(
    const Material& m, const glm::vec3& normal, const glm::vec3& wo, glm::vec3& wi) {
    float etaA = 1.0f, etaB = m.indexOfRefraction;
    float ni = dot(wo, normal);
    glm::vec3 nor = normal;

    bool entering = ni < 0;
    float etaI = entering ? etaA : etaB;
    float etaT = entering ? etaB : etaA;
    if (!entering) nor = -nor;

    // total internal reflection
    wi = etaI / etaT * sqrtf(fmaxf(0.0f, 1.0f - ni * ni)) > 1.0f ?
        glm::normalize(glm::reflect(wo, nor)) :
        glm::normalize(glm::refract(wo, nor, etaI / etaT));
    return m.specular.color / abs(glm::dot(wi, nor));
}

__host__ __device__ glm::vec3 sampleFresnelSpecularMaterial(
    const Material& m, const glm::vec3& normal, 
    const glm::vec3& wo, glm::vec3& wi, thrust::default_random_engine& rng) {

    glm::vec3 nor = normal;
    float cosThetaI = abs(glm::dot(wo, normal));
    float F = fresnelDielectricEval(-cosThetaI, 1.0f, m.indexOfRefraction);

    thrust::uniform_real_distribution<float> u01(0, 1);
    float u = u01(rng);

    if (u < F) {
        // reflect
        return sampleSpecularReflectMaterial(m, normal, wo, wi);
    }
    else {
        // transmission
        return sampleSpecularTransmissionMaterial(m, normal, wo, wi);
    }
}

__host__ __device__ glm::vec3 sampleMaterial(glm::vec3 intersect,
    glm::vec3 normal,
    glm::vec3 tangent,
    const Material& m,
    const glm::vec3& wo,
    glm::vec3& wi,
    float& pdf,
    bool& specular,
    thrust::default_random_engine& rng) {

    pdf = 1.0;
    specular = true;
    if (m.hasReflective > 0.0 && m.hasRefractive > 0.0)
    {
        return sampleFresnelSpecularMaterial(m, normal, wo, wi, rng);
    }
    else if (m.hasReflective > 0.0)
    {
        // perfect specular
        return sampleSpecularReflectMaterial(m, normal, wo, wi);

        // imperfect
        //float x1 = u01(rng), x2 = u01(rng);
        //float theta = acos(pow(x1, 1.0 / (m.specular.exponent + 1)));
        //float phi = 2 * PI * x2;

        //glm::vec3 s = glm::vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));

        //// sample direction must be transformed to world space
        //// tangent-space to world-space: local tangent, binormal, and normal at the surface point
        //glm::vec3 binormal = glm::normalize(glm::cross(normal, tangent));
        //glm::mat3 TBN = glm::mat3(tangent, binormal, normal);

        //glm::vec3 r = glm::normalize(glm::transpose(TBN) * pathSegment.ray.direction); // world-space to tangent-space

        //// specular-space to tangent-space
        //glm::mat3 sampleToTangent;
        //sampleToTangent[2] = r;
        //sampleToTangent[1] = glm::normalize(glm::vec3(-r.y, r.x, 0.0f));
        //sampleToTangent[0] = glm::normalize(glm::cross(sampleToTangent[1], sampleToTangent[2]));

        //// specular-space to world-space
        //glm::mat3 mat = TBN * sampleToTangent;

        //pathSegment.ray.direction = mat * s;
    }
    else if (m.hasRefractive > 0.0)
    {
        return sampleSpecularTransmissionMaterial(m, normal, wo, wi);
    }
    else
    {
        // diffuse
        specular = false;
        wi = glm::normalize(calculateRandomDirectionInHemisphere(normal, rng, pdf));
        return (m.color / PI);
    }
}

/**
 * Scatter a ray with some probabilities according to the material properties.
 * For example, a diffuse surface scatters in a cosine-weighted hemisphere.
 * A perfect specular surface scatters in the reflected ray direction.
 * In order to apply multiple effects to one surface, probabilistically choose
 * between them.
 *
 * The visual effect you want is to straight-up add the diffuse and specular
 * components. You can do this in a few ways. This logic also applies to
 * combining other types of materias (such as refractive).
 *
 * - Always take an even (50/50) split between a each effect (a diffuse bounce
 *   and a specular bounce), but divide the resulting color of either branch
 *   by its probability (0.5), to counteract the chance (0.5) of the branch
 *   being taken.
 *   - This way is inefficient, but serves as a good starting point - it
 *     converges slowly, especially for pure-diffuse or pure-specular.
 * - Pick the split based on the intensity of each material color, and divide
 *   branch result by that branch's probability (whatever probability you use).
 *
 * This method applies its changes to the Ray parameter `ray` in place.
 * It also modifies the color `color` of the ray in place.
 *
 * You may need to change the parameter list for your purposes!
 */
__host__ __device__
void scatterRay(
        PathSegment & pathSegment,
        glm::vec3 intersect,
        glm::vec3 normal,
        glm::vec3 tangent,
        const Material &m,
        thrust::default_random_engine &rng) {
    // TODO: implement this.
    // A basic implementation of pure-diffuse shading will just call the
    // calculateRandomDirectionInHemisphere defined above.

    float pdf = 1.0;
    bool specular = false;
    glm::vec3 dir = pathSegment.ray.direction;

    pathSegment.color *= sampleMaterial(intersect, normal, tangent, m, dir, pathSegment.ray.direction, pdf, specular, rng);
    pathSegment.color *= abs(glm::dot(glm::normalize(pathSegment.ray.direction), normal)) / pdf;

    --pathSegment.remainingBounces;
    pathSegment.ray.origin = intersect + 0.001f * pathSegment.ray.direction;
}

// sample lights
__host__ __device__ glm::vec3 sampleAreaLight(const Light& light, glm::vec3& view_point, glm::vec3& view_nor,
    int num_lights, glm::vec3& wiW, float& pdf, Geom* geoms, int geom_size, Material* materials, thrust::default_random_engine& rng) {

    thrust::uniform_real_distribution<float> u01(0, 1);
    switch (light.geom.type)
    {
        case CUBE:
            glm::mat4 t = light.geom.transform;
            glm::vec4 sample = glm::vec4(u01(rng) - 0.5f, 0.0f, u01(rng) - 0.5f, 1.0f);
            glm::vec4 light_point_W = t * sample;
            glm::vec3 light_nor_W = glm::normalize(multiplyMV(light.geom.invTranspose, glm::vec4(0., 1., 0., 0.)));

            // Compute and Convert Pdf
            glm::vec3 dis = glm::vec3(light_point_W) - view_point;
            float r = length(dis);
            float costheta = abs(dot(-normalize(dis), light_nor_W));
            pdf = r * r / (costheta * light.geom.scale.x * light.geom.scale.z * (float)num_lights);

            // Set ��i to the normalized vector from the reference point
            // to the generated light source point
            wiW = normalize(dis);

            // Check to see if ��i reaches the light source
            Ray shadowRay;
            shadowRay.origin = view_point + 0.01f * wiW;
            shadowRay.direction = wiW;

            ShadeableIntersection intersection;

            computeRayIntersection(geoms, geom_size, shadowRay, intersection);

            if (intersection.t >= 0.0f && intersection.geomId == light.geom.geomId) {
                return materials[intersection.materialId].color * materials[intersection.materialId].emittance;
            }
    }

    return glm::vec3(0.0);
}

__host__ __device__ glm::vec3 sampleLight(
    glm::vec3 intersect,
    glm::vec3 normal,
    glm::vec3& wi,
    Light& chosenLight,
    thrust::default_random_engine& rng,
    float& lightPDF,
    Geom* geoms,
    int num_geoms,
    Material* materials,
    const Light* lights,
    const int& num_lights) {

    thrust::uniform_real_distribution<float> u01(0, 1);
    int chosenLightIndex = (int)(u01(rng) * num_lights);

    chosenLight = lights[chosenLightIndex];

    switch (chosenLight.lightType)
    {
        case LightType::AREA:
		{
			return sampleAreaLight(chosenLight, intersect, normal, num_lights, wi, lightPDF, geoms, num_geoms, materials, rng);
		}
        
    }

    return glm::vec3(0.0f);
}