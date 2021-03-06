#include "MovingSphere.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Movement/Camera/ExpSpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "DrawDebugHelpers.h"
#include "Particles/ParticleSystemComponent.h"
#include "Kismet/GameplayStatics.h"

AMovingSphere::AMovingSphere()
{
 	// Set this pawn to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

	// Set this pawn to be controlled by the lowest-numbered player.
	AutoPossessPlayer = EAutoReceiveInput::Player0;

	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("RootComponent"));
	RootComponent = Body;

	CameraBoom = CreateDefaultSubobject<UExpSpringArmComponent>(TEXT("Camera Boom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 300.0f;
	CameraBoom->bUsePawnControlRotation = true;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;

	Body->SetSimulatePhysics(true);
	Body->BodyInstance.SetMassOverride(10.0f);
	Body->BodyInstance.bLockXRotation = true;
	Body->BodyInstance.bLockYRotation = true;
	Body->BodyInstance.bLockZRotation = true;

	PassiveGrassAffectorParticles = CreateDefaultSubobject<UParticleSystemComponent>(TEXT("Passive GrassAffector Particles"));
	PassiveGrassAffectorParticles->SetupAttachment(Body);
	PassiveGrassAffectorParticles->ComponentTags.Add(FName("GrassAffector"));
}

void AMovingSphere::BeginPlay()
{
	Super::BeginPlay();

	minGroundDotProduct = FMath::Cos(FMath::DegreesToRadians(maxGroundAngle));

	material = UMaterialInstanceDynamic::Create(Body->GetMaterial(0), this);
	Body->SetMaterial(0, material);
}

void AMovingSphere::SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	InputComponent->BindAxis("MoveX", this, &AMovingSphere::Move_XAxis);
	InputComponent->BindAxis("MoveY", this, &AMovingSphere::Move_YAxis);

	InputComponent->BindAction("Jump", IE_Pressed, this, &AMovingSphere::InputJump);

	// Two variations of rotation bindings to handle different kinds of devices.
	// "turn" handles devices with an absolute delta, such as mouse.
	// "turnrate" is for devices we choose to treat as a rate of change, such as a stick.
	InputComponent->BindAxis("Turn", this, &APawn::AddControllerYawInput);
	InputComponent->BindAxis("TurnRate", this, &AMovingSphere::TurnAtRate);
	InputComponent->BindAxis("LookUp", this, &APawn::AddControllerPitchInput);
	InputComponent->BindAxis("LookUpRAte", this, &AMovingSphere::LookUpAtRate);
}

void AMovingSphere::TurnAtRate(float Rate)
{
	AddControllerYawInput(Rate * BaseTurnRate * GetWorld()->GetDeltaSeconds());
}

void AMovingSphere::LookUpAtRate(float Rate)
{
	AddControllerPitchInput(Rate * BaseTurnRate * GetWorld()->GetDeltaSeconds());
}

void AMovingSphere::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UpdateState();
	AdjustVelocity(DeltaTime);

	ProbeGround();
	
	if (desiredJump)
	{
		desiredJump = false;
		Jump();
	}

	Body->SetPhysicsLinearVelocity(velocity);

	ClearState();
}

void AMovingSphere::UpdateState()
{
	velocity = Body->GetPhysicsLinearVelocity();

	stepsSinceLastGrounded++;
	stepsSinceLastJump++;

	ProbeGround();

	if (GetGrounded() || SnapToGround()) 
	{
		stepsSinceLastGrounded = 0;
		jumpPhase = 0;

		if (groundContactCount > 1)
		{
			contactNormal.Normalize();
		}

		PassiveGrassAffectorParticles->SetActive(true);
	}
	else
	{
		contactNormal = FVector::UpVector;

		PassiveGrassAffectorParticles->SetActive(false);
	}
}

void AMovingSphere::AdjustVelocity(const float DeltaTime)
{
	// Project right and forward vectors onto contact plane.
	FVector xAxis = FVector::VectorPlaneProject(FVector::ForwardVector, contactNormal);
	FVector yAxis = FVector::VectorPlaneProject(FVector::RightVector, contactNormal);
	xAxis.Normalize();
	yAxis.Normalize();

	// Project the current velocity to get the relative XY speeds.
	float currentX = FVector::DotProduct(velocity, xAxis);
	float currentY = FVector::DotProduct(velocity, yAxis);

	// Get input and desired velocity
	input = input.GetClampedToMaxSize(1.f);
	FVector desiredVelocity = FVector(input.X, input.Y, 0.f) * MaxSpeed;

	// Orient desired velocity to camera perspective.
	desiredVelocity = Camera->GetComponentTransform().TransformVector(desiredVelocity);
	desiredVelocity.Z = 0.0f;
	desiredVelocity = desiredVelocity.GetSafeNormal()* MaxSpeed;

	// Calculate new X and Y speeds relative to the ground.
	float acceleration = GetGrounded() ? GroundAcceleration : AirAcceleration;
	float newX = FMath::FInterpConstantTo(currentX, desiredVelocity.X, DeltaTime, acceleration);
	float newY = FMath::FInterpConstantTo(currentY, desiredVelocity.Y, DeltaTime, acceleration);

	// Adjust velocity by adding the differences between the new and old speeds along the relative axes.
	velocity += xAxis * (newX - currentX) + yAxis * (newY - currentY);
}

void AMovingSphere::ClearState()
{
	groundContactCount = 0;
	steepContactCount = 0;
	contactNormal = steepNormal = FVector::ZeroVector;
}

bool AMovingSphere::GotMovementInput() const
{
	return !input.IsNearlyZero();
}

ACameraModificationVolume* AMovingSphere::GetCurrentCameraModificationVolume()
{
	return CurrentCameraVolume;
}

void AMovingSphere::SetCurrentCameraModificationVolume(ACameraModificationVolume* Volume)
{
	CurrentCameraVolume = Volume;
}

FVector AMovingSphere::GetPosition()
{
	return GetActorLocation() + FVector::UpVector * 50.f;
}

void AMovingSphere::Jump()
{
	FVector jumpDirection;
	
	if (GetGrounded())
	{
		jumpDirection = contactNormal;
	}
	else if (GetOnSteep())
	{
		jumpDirection = steepNormal;
		jumpPhase = 0;
	}
	else if (jumpPhase <= MaxAirJumps && jumpPhase != 0)
	{
		jumpDirection = contactNormal;
	}
	else
	{
		return;
	}

	stepsSinceLastJump = 0;
	jumpPhase++;

	// Add upward bias to jump direction
	jumpDirection = (jumpDirection + FVector::UpVector).GetSafeNormal();

	float gravity = GetWorld()->GetGravityZ();
	float jumpSpeed = FMath::Sqrt(-2.0f * gravity * JumpHeight);
	float alignedSpeed = FVector::DotProduct(velocity, jumpDirection);

	// Limit upward velocity if jumping in quick succession.
	// NOTE: Is this actually desired?
	if (alignedSpeed > 0.f)
	{
		jumpSpeed = FMath::Max(jumpSpeed - alignedSpeed, 0.f);
	}

	velocity += jumpDirection * jumpSpeed;
}

bool AMovingSphere::GetGrounded()
{
	return groundContactCount > 0;
}

bool AMovingSphere::GetOnSteep()
{
	return steepContactCount > 0;
}

bool AMovingSphere::SnapToGround()
{
	if (stepsSinceLastGrounded > 1 || stepsSinceLastJump <= 2)
	{
		return false;
	}

	float speed = velocity.Size();
	if (speed > MaxSnapSpeed)
	{
		return false;
	}

	// Raycast to find ground surface
	FHitResult Hit;
	FCollisionQueryParams TraceParams(FName(TEXT("SnapToGround Trace")), false, this);
	FVector direction = (FVector::DownVector * ProbeDistance);

	if (GetWorld()->LineTraceSingleByChannel(
		OUT Hit,
		GetPosition(),
		GetPosition() + direction,
		GroundChannel,
		TraceParams
	))
	{
		if (Hit.Normal.Z < minGroundDotProduct)
		{
			return false;
		}

		// If we reach here, we've just lost contact with the ground, but are
		// above ground just below us.
		groundContactCount = 1;
		contactNormal = Hit.Normal;

		// Adjust velocity to align with the ground.
		float dot = FVector::DotProduct(velocity, Hit.Normal);

		// Only re-align if we are moving up away from the surface.
		if (dot > 0.f)
		{
			velocity = (velocity - Hit.Normal * dot).GetSafeNormal() * speed;
		}

		return true;
	}

	return false;
}

void AMovingSphere::ProbeGround()
{
	const uint16 num = 20;
	uint16 division = 360.f / num;

	for (uint16 z = 0; z < num- 1; z++)
	{
		for (uint16 i = 0; i < num - 1; i++)
		{
			FVector direction = FQuat::MakeFromEuler(FVector(0.f, i * division, z * division)).Vector().GetSafeNormal();

			// Test for surfaces
			{
				FHitResult Hit;
				FCollisionQueryParams TraceParams(FName(TEXT("Ground Trace")), false, this);

				if (GetWorld()->LineTraceSingleByChannel(
					OUT Hit,
					GetPosition(), 
					GetPosition() + direction * Skin,
					GroundChannel,
					TraceParams))
				{
					if (Hit.Normal.Z >= minGroundDotProduct)
					{
						groundContactCount++;
						contactNormal += Hit.Normal;
					}
					else if (Hit.Normal.Z > -0.01f)
					{
						steepContactCount++;
						steepNormal += Hit.Normal;
					}
				}
			}
		}
	}
}

void AMovingSphere::Move_XAxis(float AxisValue)
{
	// Move at 100 units per second forward of backward
	float force = FMath::Clamp(AxisValue, -1.0f, 1.0f);
	input.X = force;
}

void AMovingSphere::Move_YAxis(float AxisValue)
{
	// Move at 100 units per second right or left.
	float force = FMath::Clamp(AxisValue, -1.0f, 1.0f);
	input.Y = force;
}

void AMovingSphere::InputJump()
{
	desiredJump = true;
}

#if WITH_EDITOR
void AMovingSphere::PostEditChangeProperty(struct FPropertyChangedEvent& args)
{
	Super::PostEditChangeProperty(args);

	FName name = args.GetPropertyName();

	if (name == GET_MEMBER_NAME_CHECKED(AMovingSphere, maxGroundAngle))
	{
		minGroundDotProduct = FMath::Cos(FMath::DegreesToRadians(maxGroundAngle));
	}
}
#endif